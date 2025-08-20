#include "MeshManager.h"
#include "Dataserver.h"
#include "TclServer.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#ifdef __APPLE__
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#else
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
#endif
#include <fcntl.h>
#include <chrono>
#include <errno.h>

MeshManager::MeshManager(Dataserver* ds, TclServer* tclserver) 
    : ds(ds), tclserver(tclserver), myStatus("idle"), httpSocket(-1), udpSocket(-1),
      httpPort(12348), discoveryPort(12346) {
}

MeshManager::~MeshManager() {
    stop();
}

void MeshManager::init(const std::string& applianceId, const std::string& name) {
    myApplianceId = applianceId;
    myName = name;
    
    std::cout << "Mesh manager initialized:" << std::endl;
    std::cout << "  Appliance ID: " << applianceId << std::endl;
    std::cout << "  Name: " << name << std::endl;
    std::cout << "  Discovery Port: " << discoveryPort << std::endl;
    std::cout << "  HTTP Port: " << httpPort << std::endl;
}

void MeshManager::setHeartbeatInterval(int seconds) {
    if (seconds < 1 || seconds > 300) {
        std::cerr << "Heartbeat interval must be between 1 and 300 seconds" << std::endl;
        return;
    }
    
    int oldInterval = heartbeatInterval.exchange(seconds);
    if (oldInterval != seconds) {
        std::cout << "Mesh heartbeat interval changed from " 
                  << oldInterval << " to " << seconds << " seconds" << std::endl;
        
        // Signal the heartbeat thread to wake up and use new interval
        intervalChanged = true;
        heartbeatCV.notify_one();
    }
}

void MeshManager::setPeerTimeoutMultiplier(int multiplier) {
    if (multiplier < 2 || multiplier > 20) {
        std::cerr << "Peer timeout multiplier must be between 2 and 20" << std::endl;
        return;
    }
    
    peerTimeoutMultiplier = multiplier;
    std::cout << "Mesh peer timeout set to " 
              << getPeerTimeoutSeconds() << " seconds "
              << "(" << multiplier << " heartbeats)" << std::endl;
}

void MeshManager::start() {
    setupUDP();
    setupHTTP();
    
    if (udpSocket >= 0) {
        heartbeatThread = std::thread([this]() {
            while (running) {
                sendHeartbeat();
                cleanupExpiredPeers();
                
                // Check if we need to broadcast updates
                if (broadcastPending.exchange(false)) {
                    broadcastMeshUpdate();
                }

            // Interruptible sleep that respects interval changes
            std::unique_lock<std::mutex> lock(heartbeatMutex);
            intervalChanged = false;
            
            // Wait for either the interval to pass or a signal to wake up
            heartbeatCV.wait_for(lock, 
                std::chrono::seconds(heartbeatInterval.load()),
                [this] { return !running || intervalChanged.load(); });

            }            
        });
        
        discoveryThread = std::thread([this]() {
            while (running) {
                listenForHeartbeats();
            }
        });
    }
    
    if (httpSocket >= 0) {
        httpThread = std::thread(&MeshManager::runHttpServer, this);
    }
    
    std::cout << "Mesh networking started successfully" << std::endl;
}

void MeshManager::setupUDP() {
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket < 0) {
        std::cerr << "UDP socket creation failed: " << strerror(errno) << std::endl;
        return;
    }
    
    // Set socket to non-blocking
    int flags = fcntl(udpSocket, F_GETFL, 0);
    fcntl(udpSocket, F_SETFL, flags | O_NONBLOCK);
    
    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        std::cerr << "Failed to enable broadcast: " << strerror(errno) << std::endl;
    }
    
    // Initialize broadcast address cache
    refreshBroadcastCache();
    
    // Bind for receiving (existing code)
    struct sockaddr_in bindAddr;
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(discoveryPort);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(udpSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        std::cerr << "UDP bind failed: " << strerror(errno) << std::endl;
        close(udpSocket);
        udpSocket = -1;
    } else {
        std::cout << "Mesh UDP socket bound to port " << discoveryPort << std::endl;
        
        // Show discovered broadcast addresses
        auto addresses = getBroadcastAddresses();
        std::cout << "Broadcasting to " << addresses.size() << " networks: ";
        for (const auto& addr : addresses) {
            std::cout << addr << " ";
        }
        std::cout << std::endl;
    }
}

void MeshManager::setupHTTP() {
    httpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (httpSocket < 0) {
        std::cerr << "HTTP socket creation failed: " << strerror(errno) << std::endl;
        return;
    }
    
    int reuse = 1;
    setsockopt(httpSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(httpPort);
    
    if (bind(httpSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "HTTP bind failed: " << strerror(errno) << std::endl;
        close(httpSocket);
        httpSocket = -1;
        return;
    }
    
    if (listen(httpSocket, 10) < 0) {
        std::cerr << "HTTP listen failed: " << strerror(errno) << std::endl;
        close(httpSocket);
        httpSocket = -1;
        return;
    }
    
    std::cout << "Mesh HTTP server bound to port " << httpPort << std::endl;
}

void MeshManager::stop() {
    if (!running) return;
    
    running = false;
    
    // Close sockets to unblock any blocking calls
    if (udpSocket >= 0) {
        close(udpSocket);
        udpSocket = -1;
    }
    if (httpSocket >= 0) {
        close(httpSocket);
        httpSocket = -1;
    }
    
    // Give threads time to exit gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Join threads
    if (heartbeatThread.joinable()) {
        heartbeatThread.join();
    }
    if (discoveryThread.joinable()) {
        discoveryThread.join();
    }
    if (httpThread.joinable()) {
        httpThread.join();
    }
    
    std::cout << "Mesh networking stopped" << std::endl;
}


std::vector<std::string> MeshManager::scanNetworkBroadcastAddresses() {
    std::vector<std::string> addresses;
    
#ifdef __APPLE__
    // macOS/BSD implementation
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            // Skip if no address or not IPv4
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            
            // Skip if interface is down or doesn't support broadcast
            if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST)) continue;
            
            // Skip loopback interfaces
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            
            // Get broadcast address
            if (ifa->ifa_broadaddr) {
                struct sockaddr_in* broadcast_addr = (struct sockaddr_in*)ifa->ifa_broadaddr;
                std::string broadcastStr = inet_ntoa(broadcast_addr->sin_addr);
                
                // Skip invalid broadcast addresses
                if (broadcastStr != "0.0.0.0") {
                    addresses.push_back(broadcastStr);
//                    std::cout << "Found broadcast address: " << broadcastStr 
//                              << " (interface: " << ifa->ifa_name << ")" << std::endl;
                }
            }
        }
        freeifaddrs(ifap);
    } else {
        std::cerr << "Failed to get network interfaces: " << strerror(errno) << std::endl;
    }
#else
    // Linux implementation
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST)) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            
            if (ifa->ifa_ifu.ifu_broadaddr) {
                struct sockaddr_in* broadcast_addr = (struct sockaddr_in*)ifa->ifa_ifu.ifu_broadaddr;
                std::string broadcastStr = inet_ntoa(broadcast_addr->sin_addr);
                
                if (broadcastStr != "0.0.0.0") {
                    addresses.push_back(broadcastStr);
//                    std::cout << "Found broadcast address: " << broadcastStr 
//                              << " (interface: " << ifa->ifa_name << ")" << std::endl;
                }
            }
        }
        freeifaddrs(ifap);
    } else {
        std::cerr << "Failed to get network interfaces: " << strerror(errno) << std::endl;
    }
#endif

    // Remove duplicates and sort
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    
    // Fallback to global broadcast if no interfaces found
    if (addresses.empty()) {
        std::cout << "No broadcast interfaces found, using global broadcast" << std::endl;
        addresses.push_back("255.255.255.255");
    }
    
    return addresses;
}

void MeshManager::refreshBroadcastCache() {
    auto now = std::chrono::steady_clock::now();
    
    std::lock_guard<std::mutex> lock(broadcastCacheMutex);
    
    // Check if we need to refresh (first time or interval expired)
    if (cachedBroadcastAddresses.empty() || 
        (now - lastNetworkScan) > NETWORK_SCAN_INTERVAL) {
        
//        std::cout << "Refreshing network interface cache..." << std::endl;
        
        auto newAddresses = scanNetworkBroadcastAddresses();
        
        // Check if addresses changed
        if (newAddresses != cachedBroadcastAddresses) {
            std::cout << "Network configuration changed:" << std::endl;
            std::cout << "  Old addresses: ";
            for (const auto& addr : cachedBroadcastAddresses) {
                std::cout << addr << " ";
            }
            std::cout << std::endl;
            
            std::cout << "  New addresses: ";
            for (const auto& addr : newAddresses) {
                std::cout << addr << " ";
            }
            std::cout << std::endl;
            
            cachedBroadcastAddresses = std::move(newAddresses);
        }
        
        lastNetworkScan = now;
    }
}

std::vector<std::string> MeshManager::getBroadcastAddresses() {
    refreshBroadcastCache();
    
    std::lock_guard<std::mutex> lock(broadcastCacheMutex);
    return cachedBroadcastAddresses; // Return copy
}

void MeshManager::sendHeartbeat() {
    if (udpSocket < 0) return;
    
    // Get current broadcast addresses (cached, refreshed every 30 seconds)
    auto broadcastAddresses = getBroadcastAddresses();
    
    // Create JSON heartbeat (existing code)
    json_t* heartbeat = json_object();
    json_object_set_new(heartbeat, "type", json_string("heartbeat"));
    json_object_set_new(heartbeat, "applianceId", json_string(myApplianceId.c_str()));
    json_object_set_new(heartbeat, "timestamp", json_integer(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    
    json_t* data = json_object();
    json_object_set_new(data, "name", json_string(myName.c_str()));
    json_object_set_new(data, "status", json_string(myStatus.c_str()));
    json_object_set_new(data, "currentExperiment", json_string(currentExperiment.c_str()));
    json_object_set_new(data, "participantId", json_string(participantId.c_str()));
    json_object_set_new(data, "webPort", json_integer(httpPort));
    
    json_object_set_new(heartbeat, "data", data);
    
    char* message = json_dumps(heartbeat, JSON_COMPACT);
    size_t messageLen = strlen(message);
    
    // Send to all broadcast addresses
    int successfulSends = 0;
    for (const auto& broadcastAddrStr : broadcastAddresses) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(discoveryPort);
        
        if (inet_aton(broadcastAddrStr.c_str(), &addr.sin_addr) != 0) {
            ssize_t result = sendto(udpSocket, message, messageLen, 0,
                                   (struct sockaddr*)&addr, sizeof(addr));
            
            if (result >= 0) {
                successfulSends++;
            } else {
                std::cerr << "Failed to send heartbeat to " << broadcastAddrStr 
                          << ": " << strerror(errno) << std::endl;
            }
        } else {
            std::cerr << "Invalid broadcast address: " << broadcastAddrStr << std::endl;
        }
    }
    
    // Optional: Log successful sends for debugging
    if (successfulSends > 0) {
        // Uncomment for debugging
        // std::cout << "Sent heartbeat to " << successfulSends << " networks" << std::endl;
    } else {
        std::cerr << "Failed to send heartbeat to any network!" << std::endl;
    }
    
    free(message);
    json_decref(heartbeat);
}

void MeshManager::listenForHeartbeats() {
    if (udpSocket < 0) return;
    
    char buffer[1024];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    
    ssize_t bytesReceived = recvfrom(udpSocket, buffer, sizeof(buffer) - 1, 0,
                                    (struct sockaddr*)&fromAddr, &fromLen);
    
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        
        json_error_t error;
        json_t* message = json_loads(buffer, 0, &error);
        
        if (message && json_is_object(message)) {
            json_t* type = json_object_get(message, "type");
            json_t* applianceId = json_object_get(message, "applianceId");
            
            if (json_is_string(type) && json_is_string(applianceId) &&
                strcmp(json_string_value(type), "heartbeat") == 0 &&
                strcmp(json_string_value(applianceId), myApplianceId.c_str()) != 0) {
                
                updatePeer(message, inet_ntoa(fromAddr.sin_addr));
            }
            
            json_decref(message);
        }
    } else {
        // No data available or error - sleep to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void MeshManager::updatePeer(json_t* heartbeat, const std::string& ip) {
    std::lock_guard<std::mutex> lock(peersMutex);
    
    json_t* applianceId = json_object_get(heartbeat, "applianceId");
    json_t* data = json_object_get(heartbeat, "data");
    
    if (!json_is_string(applianceId) || !json_is_object(data)) return;
    
    std::string peerId = json_string_value(applianceId);
    PeerInfo& peer = peers[peerId];
    
    peer.applianceId = peerId;
    peer.ipAddress = ip;
    peer.lastHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    json_t* name = json_object_get(data, "name");
    if (json_is_string(name)) peer.name = json_string_value(name);
    
    json_t* status = json_object_get(data, "status");
    if (json_is_string(status)) peer.status = json_string_value(status);
    
    json_t* experiment = json_object_get(data, "currentExperiment");
    if (json_is_string(experiment)) peer.currentExperiment = json_string_value(experiment);
    
    json_t* participant = json_object_get(data, "participantId");
    if (json_is_string(participant)) peer.participantId = json_string_value(participant);
    
    json_t* webPort = json_object_get(data, "webPort");
    if (json_is_integer(webPort)) peer.webPort = json_integer_value(webPort);
    
    broadcastPending = true;
}

void MeshManager::cleanupExpiredPeers() {
    bool peersRemoved = false;
    {
        std::lock_guard<std::mutex> lock(peersMutex);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Use the configurable timeout
        int timeoutMs = getPeerTimeoutSeconds() * 1000;
        
        auto it = peers.begin();
        while (it != peers.end()) {
            if (now - it->second.lastHeartbeat > timeoutMs) {
//                std::cout << "Peer " << it->first << " (" << it->second.name 
//                          << ") timed out after " << getPeerTimeoutSeconds() 
//                          << " seconds" << std::endl;
                it = peers.erase(it);
                peersRemoved = true;
            } else {
                ++it;
            }
        }
    }
    
    if (peersRemoved) {
        broadcastPending = true;  // Will broadcast on next heartbeat cycle
    }
}

void MeshManager::updateStatus(const std::string& status) {
    if (myStatus != status) {
        myStatus = status;
	//        std::cout << "Mesh status updated to: " << status << std::endl;
        sendHeartbeat();
        broadcastMeshUpdate();
    }
}


void MeshManager::updateExperiment(const std::string& experiment) {
    if (currentExperiment != experiment) {
        currentExperiment = experiment;
	//        std::cout << "Mesh experiment updated to: " << experiment << std::endl;
        sendHeartbeat();
        broadcastMeshUpdate();
    }
}

void MeshManager::updateParticipant(const std::string& participant) {
    if (participantId != participant) {
        participantId = participant;
	//        std::cout << "Mesh participant updated to: " << participant << std::endl;
        sendHeartbeat();
        broadcastMeshUpdate();
    }
}

std::vector<MeshManager::PeerInfo> MeshManager::getPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<PeerInfo> peerList;
    
    for (const auto& [id, peer] : peers) {
        peerList.push_back(peer);
    }
    
    return peerList;
}

std::string MeshManager::getPeersJSON() {
    std::lock_guard<std::mutex> lock(peersMutex);
    
    json_t* result = json_object();
    json_t* appliances_array = json_array();
    
    // Add self
    json_t* self = json_object();
    json_object_set_new(self, "applianceId", json_string(myApplianceId.c_str()));
    json_object_set_new(self, "name", json_string(myName.c_str()));
    json_object_set_new(self, "status", json_string(myStatus.c_str()));
    json_object_set_new(self, "currentExperiment", json_string(currentExperiment.c_str()));
    json_object_set_new(self, "participantId", json_string(participantId.c_str()));
    json_object_set_new(self, "isLocal", json_true());
    json_array_append_new(appliances_array, self);
    
    // Add peers
    for (const auto& [id, peer] : peers) {
        json_t* peerObj = json_object();
        json_object_set_new(peerObj, "applianceId", json_string(peer.applianceId.c_str()));
        json_object_set_new(peerObj, "name", json_string(peer.name.c_str()));
        json_object_set_new(peerObj, "status", json_string(peer.status.c_str()));
        json_object_set_new(peerObj, "currentExperiment", json_string(peer.currentExperiment.c_str()));
        json_object_set_new(peerObj, "participantId", json_string(peer.participantId.c_str()));
        json_object_set_new(peerObj, "ipAddress", json_string(peer.ipAddress.c_str()));
        json_object_set_new(peerObj, "webPort", json_integer(peer.webPort));
        json_object_set_new(peerObj, "isLocal", json_false());
        json_array_append_new(appliances_array, peerObj);
    }
    
    json_object_set_new(result, "appliances", appliances_array);
    
    char* resultStr = json_dumps(result, 0);
    std::string returnValue(resultStr);
    free(resultStr);
    json_decref(result);
    
    return returnValue;
}

void MeshManager::runHttpServer() {
    if (httpSocket < 0) return;
    
    while (running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        
        int clientSocket = accept(httpSocket, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSocket < 0) {
            if (running) {  // Only print error if we're still supposed to be running
                if (errno != EBADF) { // Don't print error if socket was closed during shutdown
                    std::cerr << "HTTP accept failed: " << strerror(errno) << std::endl;
                }
            }
            continue;
        }
        
        // Handle request in separate thread to avoid blocking
        std::thread([this, clientSocket]() {
            handleHttpRequest(clientSocket);
            close(clientSocket);
        }).detach();
    }
}

void MeshManager::handleHttpRequest(int clientSocket) {
    char buffer[1024];
    ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytesRead <= 0) {
        return;
    }
    
    buffer[bytesRead] = '\0';
    std::string request(buffer);
    
    // Parse HTTP request line
    size_t firstLine = request.find('\n');
    std::string requestLine = request.substr(0, firstLine);
    
    std::istringstream iss(requestLine);
    std::string method, path, version;
    iss >> method >> path >> version;
    
    std::string response;
    std::string contentType = "text/html";
    std::string httpStatus = "200 OK";
    
    if (path == "/mesh" || path == "/") {
        response = getMeshHTML();
    } else if (path == "/api/mesh/peers") {
        response = getPeersJSON();
        contentType = "application/json";
    } else {
        response = "404 Not Found";
        contentType = "text/plain";
        httpStatus = "404 Not Found";
    }
    
    std::string httpResponse = "HTTP/1.1 " + httpStatus + "\r\n";
    httpResponse += "Content-Type: " + contentType + "\r\n";
    httpResponse += "Content-Length: " + std::to_string(response.length()) + "\r\n";
    httpResponse += "Access-Control-Allow-Origin: *\r\n";
    httpResponse += "\r\n";
    httpResponse += response;
    
    send(clientSocket, httpResponse.c_str(), httpResponse.length(), 0);
}

std::string MeshManager::getMeshHTML() {
    std::string fallback = "<html><body>";
    fallback += "<h1>Mesh Dashboard - " + myName + "</h1>";
    fallback += "<p>Status: " + myStatus + "</p>";
    fallback += "<p>Appliance ID: " + myApplianceId + "</p>";
    fallback += "<button onclick=\"fetch('/api/mesh/peers').then(r=>r.json()).then(d=>console.log(d))\">Test API</button>";
    fallback += "<p><a href=\"/api/mesh/peers\">View JSON API</a></p>";
    fallback += "</body></html>";
    return fallback;
}

void MeshManager::addMeshSubscriber(uWS::WebSocket<false, true, WSPerSocketData>* ws) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    meshSubscribers.insert(ws);
    //    std::cout << "Added mesh WebSocket subscriber (total: " << meshSubscribers.size() << ")" << std::endl;
}

void MeshManager::removeMeshSubscriber(uWS::WebSocket<false, true, WSPerSocketData>* ws) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    meshSubscribers.erase(ws);
    //    std::cout << "Removed mesh WebSocket subscriber (total: " << meshSubscribers.size() << ")" << std::endl;
}

void MeshManager::broadcastMeshUpdate() {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
    if (meshSubscribers.empty()) return;
    
    json_t* update = json_object();
    json_object_set_new(update, "type", json_string("mesh_update"));
    json_object_set_new(update, "data", json_loads(getPeersJSON().c_str(), 0, nullptr));
    
    char* message = json_dumps(update, 0);
    std::string messageStr(message);
    
    // Broadcast to all subscribers
    auto it = meshSubscribers.begin();
    while (it != meshSubscribers.end()) {
        try {
            bool sent = (*it)->send(messageStr, uWS::OpCode::TEXT);
            if (!sent) {
                it = meshSubscribers.erase(it);
                continue;
            }
        } catch (...) {
            it = meshSubscribers.erase(it);
            continue;
        }
        ++it;
    }
    
    free(message);
    json_decref(update);
}

void MeshManager::broadcastCustomUpdate(const std::string& standardJson, const std::string& customJson) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
    if (meshSubscribers.empty()) return;
    
    json_t* update = json_object();
    json_object_set_new(update, "type", json_string("mesh_custom_update"));
    
    // Parse the JSON strings from Tcl
    json_error_t error;
    json_t* standardData = json_loads(standardJson.c_str(), 0, &error);
    json_t* customData = json_loads(customJson.c_str(), 0, &error);
    
    if (standardData) json_object_set_new(update, "standardData", standardData);
    if (customData) json_object_set_new(update, "customData", customData);
    
    char* message = json_dumps(update, 0);
    std::string messageStr(message);
    
    // Broadcast to all subscribers
    auto it = meshSubscribers.begin();
    while (it != meshSubscribers.end()) {
        try {
            bool sent = (*it)->send(messageStr, uWS::OpCode::TEXT);
            if (!sent) {
                it = meshSubscribers.erase(it);
                continue;
            }
        } catch (...) {
            it = meshSubscribers.erase(it);
            continue;
        }
        ++it;
    }
    
    free(message);
    json_decref(update);
}

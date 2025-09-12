#include "MeshDiscovery.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>
#include <errno.h>

#include <jansson.h>

MeshDiscovery::MeshDiscovery(int discoveryPort) 
    : discoveryPort(discoveryPort), meshSocket(-1) {
}

MeshDiscovery::~MeshDiscovery() {
    closeSocket();
}

int MeshDiscovery::discoverPeers(int timeoutMs) {
    if (createSocket() < 0) {
        return -1;
    }
    
    auto startTime = std::chrono::steady_clock::now();
    auto timeoutDuration = std::chrono::milliseconds(timeoutMs);
    
    //    std::cout << "Listening for mesh heartbeats for " << (timeoutMs / 1000.0) << " seconds..." << std::endl;
    
    while (std::chrono::steady_clock::now() - startTime < timeoutDuration) {
        char buffer[1024];
        struct sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        
        ssize_t bytesReceived = recvfrom(meshSocket, buffer, sizeof(buffer) - 1, 0,
                                        (struct sockaddr*)&fromAddr, &fromLen);
        
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            processMeshHeartbeat(buffer, inet_ntoa(fromAddr.sin_addr));
        } else {
            // Brief sleep to avoid busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    closeSocket();
    cleanupExpiredPeers();
    
    //    std::cout << "Mesh discovery complete, found " << getPeerCount() << " peers" << std::endl;
    return 0;
}

std::vector<MeshDiscovery::PeerInfo> MeshDiscovery::getPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<PeerInfo> peerList;
    
	for (const auto& entry : discoveredPeers) {
		peerList.push_back(entry.second);
	}

    // Sort by name, then by IP
    std::sort(peerList.begin(), peerList.end(), 
        [](const PeerInfo& a, const PeerInfo& b) {
            if (a.name != b.name) {
                return a.name < b.name;
            }
            return a.ipAddress < b.ipAddress;
        });
    
    return peerList;
}

std::vector<std::string> MeshDiscovery::getPeerAddresses() const {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<std::string> addresses;
    
    for (const auto& entry : discoveredPeers) {
        // Skip localhost variants since we handle that separately
        if (entry.second.ipAddress != "127.0.0.1" && entry.second.ipAddress != "localhost") {
            addresses.push_back(entry.second.ipAddress);
        }
    }
    
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    
    return addresses;
}

std::vector<std::string> MeshDiscovery::getPeerDisplayTexts() const {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<std::string> displayTexts;
    
    for (const auto& entry : discoveredPeers) {
        // Skip localhost variants
        if (entry.second.ipAddress != "127.0.0.1" && entry.second.ipAddress != "localhost") {
            displayTexts.push_back(entry.second.getDisplayText());
        }
    }
    
    std::sort(displayTexts.begin(), displayTexts.end());
    
    return displayTexts;
}

size_t MeshDiscovery::getPeerCount() const {
    std::lock_guard<std::mutex> lock(peersMutex);
    return discoveredPeers.size();
}

bool MeshDiscovery::isLocalhostAvailable() const {
    return testLocalhostConnection();
}

void MeshDiscovery::cleanupExpiredPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    auto it = discoveredPeers.begin();
    while (it != discoveredPeers.end()) {
        if (now - it->second.lastSeen > PEER_TIMEOUT_MS) {
            std::cout << "Peer " << it->second.name << " (" << it->second.applianceId 
                      << ") timed out" << std::endl;
            it = discoveredPeers.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshDiscovery::clearPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    discoveredPeers.clear();
}

void MeshDiscovery::setDiscoveryCallback(std::function<void(const PeerInfo&)> callback) {
    discoveryCallback = callback;
}

std::string MeshDiscovery::extractIpFromDisplayText(const std::string& displayText) {
    // Handle "Name (IP)" format
    if (displayText.find(" (") != std::string::npos && displayText.back() == ')') {
        size_t start = displayText.find_last_of('(') + 1;
        size_t end = displayText.find_last_of(')');
        if (start < end) {
            return displayText.substr(start, end - start);
        }
    }
    
    // Return as-is if it's just an IP or localhost
    return displayText;
}

void MeshDiscovery::processMeshHeartbeat(const char* data, const char* senderIP) {
    json_error_t error;
    json_t* message = json_loads(data, 0, &error);
    
    if (!message) {
        return; // Invalid JSON, ignore silently
    }
    
    json_t* type = json_object_get(message, "type");
    json_t* applianceId = json_object_get(message, "applianceId");
    json_t* heartbeatData = json_object_get(message, "data");
    
    if (!json_is_string(type) || !json_is_string(applianceId) || !json_is_object(heartbeatData)) {
        json_decref(message);
        return;
    }
    
    if (strcmp(json_string_value(type), "heartbeat") != 0) {
        json_decref(message);
        return;
    }
    
    std::string peerId = json_string_value(applianceId);
    
    // Clean up IPv6-mapped IPv4 addresses (::ffff:192.168.x.x -> 192.168.x.x)
    std::string cleanIP = senderIP;
    if (cleanIP.find("::ffff:") == 0) {
        cleanIP = cleanIP.substr(7);
    }
    
    {
        std::lock_guard<std::mutex> lock(peersMutex);
        
        // Check if this is a new peer
        bool isNewPeer = (discoveredPeers.find(peerId) == discoveredPeers.end());
        
        // Create or update peer info
        PeerInfo& peer = discoveredPeers[peerId];
        peer.applianceId = peerId;
        peer.ipAddress = cleanIP;
        peer.lastSeen = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        // Clear custom fields before updating
        peer.customFields.clear();
        
        // Extract standard fields
        json_t* name = json_object_get(heartbeatData, "name");
        json_t* status = json_object_get(heartbeatData, "status");
        json_t* webPort = json_object_get(heartbeatData, "webPort");
        
        if (json_is_string(name)) {
            peer.name = json_string_value(name);
        } else {
            peer.name = cleanIP; // Fallback to IP if no name
        }
        
        if (json_is_string(status)) {
            peer.status = json_string_value(status);
        } else {
            peer.status = "unknown";
        }
        
        if (json_is_integer(webPort)) {
            peer.webPort = json_integer_value(webPort);
        }
        
        // Store all other fields as custom fields
        const char* key;
        json_t* value;
        json_object_foreach(heartbeatData, key, value) {
            if (json_is_string(value)) {
                std::string keyStr = key;
                if (keyStr != "name" && keyStr != "status" && keyStr != "webPort") {
                    peer.customFields[keyStr] = json_string_value(value);
                }
            }
        }
        
        // Log new peer discovery
        if (isNewPeer) {
	  //            std::cout << "Discovered mesh peer: " << peer.name << " (" << peerId 
          //            << ") at " << cleanIP << " - " << peer.status << std::endl;
            
            // Call discovery callback if set
            if (discoveryCallback) {
                discoveryCallback(peer);
            }
        }
    }
    
    json_decref(message);
}

int MeshDiscovery::createSocket() {
    closeSocket();
    
    meshSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (meshSocket < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return -1;
    }
    
    // MUST set these BEFORE bind on macOS
    int yes = 1;
    
    // Set REUSEADDR first
    if (setsockopt(meshSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << strerror(errno) << std::endl;
    }
    
#ifdef SO_REUSEPORT  // Changed from __APPLE__ to SO_REUSEPORT
    // On macOS, MUST set SO_REUSEPORT for multiple binds to same port
    if (setsockopt(meshSocket, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
        std::cerr << "Warning: SO_REUSEPORT failed: " << strerror(errno) << std::endl;
        // Don't return -1 here - continue anyway
    }
#endif
    
    // Enable broadcast BEFORE bind
    if (setsockopt(meshSocket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        std::cerr << "setsockopt SO_BROADCAST failed: " << strerror(errno) << std::endl;
    }
    
    // NOW bind
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(discoveryPort);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(meshSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        std::cerr << "Bind failed on port " << discoveryPort << ": " << strerror(errno) << std::endl;
        closeSocket();
        return -1;
    }
    
    // Set non-blocking AFTER successful bind
    int flags = fcntl(meshSocket, F_GETFL, 0);
    fcntl(meshSocket, F_SETFL, flags | O_NONBLOCK);
    
    std::cout << "Successfully bound to port " << discoveryPort << std::endl;
    return 0;
}

void MeshDiscovery::closeSocket() {
    if (meshSocket >= 0) {
        close(meshSocket);
        meshSocket = -1;
    }
}

bool MeshDiscovery::testLocalhostConnection() const {
    // Quick test to see if dserv is running locally
    int testSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (testSocket < 0) {
        return false;
    }
    
    // Set socket to non-blocking for quick test
    int flags = fcntl(testSocket, F_GETFL, 0);
    fcntl(testSocket, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOCALHOST_TEST_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    int result = connect(testSocket, (struct sockaddr*)&addr, sizeof(addr));
    close(testSocket);
    
    // Connection succeeded immediately, or is in progress
    return (result == 0 || errno == EINPROGRESS);
}

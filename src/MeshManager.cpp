#include "MeshManager.h"
#include "Dataserver.h"
#include "TclServer.h"
#include <iostream>
#include <future>
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

// TclServer
#include "TclServer.h"

// Mesh 
#include "MeshManager.h"

// WebSocket
#include <App.h>
#include "embedded_mesh_dashboard.h"


MeshManager::MeshManager(Dataserver* ds, int argc, char *argv[],
int http_port, int discovery_port, int websocket_port) 
    : ds(ds), argc(argc), argv(argv), myStatus("idle"), httpSocket(-1), udpSocket(-1),
      httpPort(http_port), discoveryPort(discovery_port), mesh_websocket_port(websocket_port) {
}

MeshManager::~MeshManager() {
    stop();  // This now handles all the thread cleanup
    
    // Handle WebSocket thread separately since it's often the problem
    if (mesh_ws_thread.joinable()) {
        std::cout << "Waiting for WebSocket thread to exit..." << std::endl;
        if (!joinThreadWithTimeout(mesh_ws_thread, std::chrono::seconds(2))) {
            std::cout << "WebSocket thread hanging, detaching..." << std::endl;
            mesh_ws_thread.detach();
        }
    }
    
    // Clean up TclServer subprocess
    if (mesh_tclserver) {
        std::cout << "Shutting down mesh TclServer..." << std::endl;
        mesh_tclserver->shutdown();
        extern ObjectRegistry<TclServer> TclServerRegistry;
        TclServerRegistry.unregisterObject("mesh");
        mesh_tclserver.reset();
    }
}

std::unique_ptr<MeshManager> MeshManager::createAndStart(
    Dataserver* ds, TclServer* main_tclserver, int argc, char** argv,
    const std::string& appliance_id, const std::string& appliance_name,
    int http_port, int discovery_port, int websocket_port) {
    
    std::cout << "Initializing mesh networking..." << std::endl;
    
    // Set defaults if needed
    std::string final_id = appliance_id;
    std::string final_name = appliance_name;
    
    if (final_id.empty()) {
        final_id = getHostname();
        std::cout << "Using default appliance ID: " << final_id << std::endl;
    }
    
    if (final_name.empty()) {
        final_name = "Lab Station " + final_id;
        std::cout << "Using default appliance name: " << final_name << std::endl;
    }
    
    std::cout << "Mesh configuration:" << std::endl;
    std::cout << "  Appliance ID: " << final_id << std::endl;
    std::cout << "  Appliance Name: " << final_name << std::endl;
    std::cout << "  HTTP Port: " << http_port << std::endl;
    std::cout << "  Discovery Port: " << discovery_port << std::endl;
    
    try {
        auto meshManager = std::make_unique<MeshManager>(ds, argc, argv, http_port, discovery_port, websocket_port);
        meshManager->init(final_id, final_name, 2575);
        meshManager->start();
        
        std::cout << "Mesh networking enabled:" << std::endl;
        std::cout << "  HTTP Dashboard: http://localhost:" << http_port << "/mesh" << std::endl;
        std::cout << "  WebSocket Dashboard: http://localhost:" << websocket_port << "/" << std::endl;
        
        // Set Tcl variables for dsconf.tcl
        std::string meshConfigScript = R"(
set ::mesh_enabled 1
set ::mesh_appliance_id ")" + final_id + R"("
set ::mesh_appliance_name ")" + final_name + R"("
set ::mesh_http_port )" + std::to_string(http_port) + R"(
set ::mesh_discovery_port )" + std::to_string(discovery_port) + R"(
set ::mesh_websocket_port )" + std::to_string(websocket_port) + R"(
)";
        
        auto result = main_tclserver->eval(meshConfigScript);
        if (result.starts_with("!TCL_ERROR ")) {
            std::cerr << "Failed to set mesh Tcl variables: " << result << std::endl;
        }
        
        return meshManager;
        
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize mesh networking: " << e.what() << std::endl;
        return nullptr;
    }
}

// Add getHostname helper
std::string MeshManager::getHostname() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }
    return "unknown";
}

void MeshManager::init(const std::string& applianceId, const std::string& name, int mesh_tcl_port) {
    myApplianceId = applianceId;
    myName = name;
    
    std::cout << "Mesh manager initialized:" << std::endl;
    std::cout << "  Appliance ID: " << applianceId << std::endl;
    std::cout << "  Name: " << name << std::endl;
    std::cout << "  Discovery Port: " << discoveryPort << std::endl;
    std::cout << "  HTTP Port: " << httpPort << std::endl;
    
    // Create TclServer with only newline port, no message port, no WebSocket
    mesh_tclserver = std::make_unique<TclServer>(argc, argv, ds, "mesh", mesh_tcl_port);
    
    // Register it with the global registry
    extern ObjectRegistry<TclServer> TclServerRegistry;
    TclServerRegistry.registerObject("mesh", mesh_tclserver.get());
    
    // Add mesh-specific Tcl commands to this subprocess
    mesh_tclserver->setCommandCallback(
        [](Tcl_Interp* interp, void* data) {
            MeshManager* mesh = static_cast<MeshManager*>(data);
            mesh->addTclCommands(interp);
        }, 
        this  // Pass MeshManager instance as callback data
    );

    std::cout << "  Created mesh TclServer subprocess on port " << mesh_tcl_port << std::endl;
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

void MeshManager::startMeshWebSocketServer(int port) {
    mesh_ws_thread = std::thread([this, port]() {
        auto app = uWS::App();
        
        // Mesh dashboard at root
        app.get("/", [this](auto *res, auto *req) {
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
              ->writeHeader("Cache-Control", "no-cache")
              ->end(embedded::mesh_dashboard_html);
        });
        
        // Dashboard alias  
        app.get("/dashboard", [](auto *res, auto *req) {
            res->writeStatus("302 Found")
              ->writeHeader("Location", "/")
              ->end();
        });
        
        // API endpoint for peers
        app.get("/api/peers", [this](auto *res, auto *req) {
            res->writeHeader("Content-Type", "application/json")
              ->writeHeader("Access-Control-Allow-Origin", "*")
              ->end(getPeersJSON());
        });
        
        // API endpoint for lost peers
		app.get("/api/lost-peers", [this](auto *res, auto *req) {
			res->writeHeader("Content-Type", "application/json")
			   ->writeHeader("Access-Control-Allow-Origin", "*")
			   ->end(getLostPeersJSON());
		});

        // Health check
        app.get("/health", [](auto *res, auto *req) {
            res->writeHeader("Content-Type", "application/json")
              ->end("{\"status\":\"ok\",\"service\":\"mesh-manager\"}");
        });
        
        // WebSocket for mesh updates
        app.ws<MeshWSData>("/ws", {
            .upgrade = [](auto *res, auto *req, auto *context) {
                res->template upgrade<MeshWSData>({
                    .rqueue = new SharedQueue<std::string>(),
                    .client_name = "",
                    .subscriptions = std::vector<std::string>(),
                    .notification_queue = nullptr,
                    .dataserver_client_id = ""
                }, req->getHeader("sec-websocket-key"),
                   req->getHeader("sec-websocket-protocol"),
                   req->getHeader("sec-websocket-extensions"),
                   context);
            },
            
            .open = [this](auto *ws) {
                MeshWSData *userData = (MeshWSData *) ws->getUserData();
                
                // Generate client name
                char client_id[32];
                snprintf(client_id, sizeof(client_id), "mesh_ws_%p", (void*)ws);
                userData->client_name = std::string(client_id);
                
                // Add to mesh subscribers automatically
                addMeshSubscriber(ws);
                
//                std::cout << "Mesh WebSocket client connected: " << userData->client_name << std::endl;
            },
            
            .message = [this](auto *ws, std::string_view message, uWS::OpCode opCode) {
                handleMeshWebSocketMessage(ws, message);
            },
            
            .close = [this](auto *ws, int code, std::string_view message) {
                MeshWSData *userData = (MeshWSData *) ws->getUserData();
                
                if (userData) {
                    removeMeshSubscriber(ws);
                    delete userData->rqueue;
//                    std::cout << "Mesh WebSocket client disconnected: " << userData->client_name << std::endl;
                }
            }
        });
        
        app.listen(port, [this, port](auto *listen_socket) {
            if (listen_socket) {
                std::cout << "Mesh WebSocket server listening on port " << port << std::endl;
                std::cout << "Mesh dashboard available at http://localhost:" << port << "/" << std::endl;
            } else {
                std::cerr << "Failed to start mesh WebSocket server on port " << port << std::endl;
                return; // Exit thread if listen failed
            }
            
            // Store the listen socket so we can close it during shutdown
            mesh_listen_socket = listen_socket;
            
        }).run(); // This blocks until the app is closed
        
        std::cout << "Mesh WebSocket server thread exiting" << std::endl;
    });
}

void MeshManager::handleMeshWebSocketMessage(auto *ws, std::string_view message) {
    // Simple JSON protocol for mesh operations
    json_error_t error;
    json_t *root = json_loads(std::string(message).c_str(), 0, &error);
    
    if (!root) {
        json_t *error_response = json_object();
        json_object_set_new(error_response, "error", json_string("Invalid JSON"));
        char *error_str = json_dumps(error_response, 0);
        ws->send(error_str, uWS::OpCode::TEXT);
        free(error_str);
        json_decref(error_response);
        return;
    }
    
    json_t *cmd_obj = json_object_get(root, "cmd");
    if (!cmd_obj || !json_is_string(cmd_obj)) {
        json_t *error_response = json_object();
        json_object_set_new(error_response, "error", json_string("Missing 'cmd' field"));
        char *error_str = json_dumps(error_response, 0);
        ws->send(error_str, uWS::OpCode::TEXT);
        free(error_str);
        json_decref(error_response);
        json_decref(root);
        return;
    }
    
    const char *cmd = json_string_value(cmd_obj);
    
    if (strcmp(cmd, "get_peers") == 0) {
        ws->send(getPeersJSON(), uWS::OpCode::TEXT);
    }
    else if (strcmp(cmd, "mesh_subscribe") == 0) {
        // Already subscribed in .open handler
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("ok"));
        json_object_set_new(response, "action", json_string("subscribed"));
        char *response_str = json_dumps(response, 0);
        ws->send(response_str, uWS::OpCode::TEXT);
        free(response_str);
        json_decref(response);
    }
    // Add other mesh-specific commands as needed
    
    json_decref(root);
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

	if (running) {  // Check first
    	std::cerr << "MeshManager already running" << std::endl;
    	return;
	}
	running = true;
	
    // Setup sockets first
    setupUDP();
    setupHTTP();
    
    startMeshWebSocketServer(mesh_websocket_port);
     
    if (udpSocket >= 0) {
        // Start heartbeat thread
		heartbeatThread = std::thread([this]() {
			while (running) {
				sendHeartbeat();  // This now includes WebSocket notification
				cleanupExpiredPeers();
				
				// Interruptible sleep
				std::unique_lock<std::mutex> lock(heartbeatMutex);
				intervalChanged = false;
				heartbeatCV.wait_for(lock, 
					std::chrono::seconds(heartbeatInterval.load()),
					[this] { return !running || intervalChanged.load(); });
			}
		});
        
        // Start discovery thread
        discoveryThread = std::thread(&MeshManager::listenForHeartbeats, this);
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
    
    // Keep socket non-blocking for all platforms
    int flags = fcntl(udpSocket, F_GETFL, 0);
    fcntl(udpSocket, F_SETFL, flags | O_NONBLOCK);
    
    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        std::cerr << "Failed to enable broadcast: " << strerror(errno) << std::endl;
    }
    
    // Enable reuse
    int reuse = 1;
    setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Scan network interfaces ONCE at startup
    cachedBroadcastAddresses = scanNetworkBroadcastAddresses();
    lastNetworkScan = std::chrono::steady_clock::now();
    
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

void MeshManager::listenForHeartbeats() {
    if (udpSocket < 0) return;
    
    char buffer[1024];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    
    while (running) {
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
            // Check for messages only twice per second - plenty for discovery
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void MeshManager::stop() {
    if (!running) return;
    
    std::cout << "Stopping mesh networking..." << std::endl;
    
    running = false;
    ws_should_stop = true;  // Signal WebSocket thread to stop
    
    // Close WebSocket listen socket to break the event loop
    if (mesh_listen_socket) {
        us_listen_socket_close(0, mesh_listen_socket);
        mesh_listen_socket = nullptr;
    }
    
    // Wake up heartbeat thread
    heartbeatCV.notify_one();
    
    // Close sockets to unblock any blocking calls
    if (udpSocket >= 0) {
        shutdown(udpSocket, SHUT_RDWR);  // More forceful than close
        close(udpSocket);
        udpSocket = -1;
    }
    if (httpSocket >= 0) {
        shutdown(httpSocket, SHUT_RDWR);  // More forceful
        close(httpSocket);
        httpSocket = -1;
    }
    
    // Join threads with individual timeouts
    if (heartbeatThread.joinable()) {
        if (!joinThreadWithTimeout(heartbeatThread, std::chrono::seconds(1))) {
            std::cerr << "Warning: Heartbeat thread didn't exit cleanly" << std::endl;
            heartbeatThread.detach();
        }
    }
    
    if (discoveryThread.joinable()) {
        if (!joinThreadWithTimeout(discoveryThread, std::chrono::seconds(1))) {
            std::cerr << "Warning: Discovery thread didn't exit cleanly" << std::endl;
            discoveryThread.detach();
        }
    }
    
    if (httpThread.joinable()) {
        if (!joinThreadWithTimeout(httpThread, std::chrono::seconds(1))) {
            std::cerr << "Warning: HTTP thread didn't exit cleanly" << std::endl;
            httpThread.detach();
        }
    }
    
    std::cout << "Mesh networking stopped" << std::endl;
}

bool MeshManager::joinThreadWithTimeout(std::thread& t, std::chrono::seconds timeout) {
    auto future = std::async(std::launch::async, [&t]() {
        if (t.joinable()) {
            t.join();
        }
    });
    
    return future.wait_for(timeout) != std::future_status::timeout;
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
    json_object_set_new(data, "webPort", json_integer(httpPort));
    
    // Everything else is custom (including system/protocol/variant and subject)
    {
        std::lock_guard<std::mutex> lock(customFieldsMutex);
        for (const auto& [key, value] : customFields) {
            json_object_set_new(data, key.c_str(), json_string(value.c_str()));
        }
    }
    
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
    
    // After sending our heartbeat, notify WebSocket clients of current state
    notifyWebSocketClients();
}

void MeshManager::notifyWebSocketClients() {
    // Prepare the current mesh state
    json_t* update = json_object();
    json_object_set_new(update, "type", json_string("mesh_update"));
    json_object_set_new(update, "data", json_loads(getPeersJSON().c_str(), 0, nullptr));
    
    char* message = json_dumps(update, 0);
    std::string messageStr(message);
    free(message);
    json_decref(update);
    
    // Send to all mesh subscribers
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
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
}

void MeshManager::updatePeer(json_t* heartbeat, const std::string& ip) {
    std::lock_guard<std::mutex> lock(peersMutex);

    json_t* applianceId = json_object_get(heartbeat, "applianceId");
    json_t* data = json_object_get(heartbeat, "data");

    if (!json_is_string(applianceId) || !json_is_object(data)) return;

    std::string peerId = json_string_value(applianceId);
    
    // Check if this peer was in the lost list
    auto lostIt = std::remove_if(lostPeers.begin(), lostPeers.end(),
        [&peerId](const LostPeerInfo& lost) {
            return lost.peer.applianceId == peerId;
        });
    
    if (lostIt != lostPeers.end()) {
 //       std::cout << "Peer " << peerId << " has reconnected" << std::endl;
        lostPeers.erase(lostIt, lostPeers.end());
    }
    
    // Check if this is a new peer or returning peer
    bool isNew = (peers.find(peerId) == peers.end());
    if (isNew && lostIt == lostPeers.end()) {
//        std::cout << "New peer discovered: " << peerId << std::endl;
    }
    
    PeerInfo& peer = peers[peerId];  // Creates if not exists

    peer.applianceId = peerId;
    peer.ipAddress = ip;
    peer.lastHeartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Reset fields
    peer.customFields.clear();

    const char* key;
    json_t* value;
    json_object_foreach(data, key, value) {
        if (json_is_string(value)) {
            std::string strValue = json_string_value(value);
            if (strcmp(key, "name") == 0) {
                peer.name = strValue;
            } else if (strcmp(key, "status") == 0) {
                peer.status = strValue;
            } else if (strcmp(key, "webPort") != 0) {
                peer.customFields[key] = strValue;
            }
        } else if (json_is_integer(value) && strcmp(key, "webPort") == 0) {
            peer.webPort = json_integer_value(value);
        }
    }
}

void MeshManager::cleanupExpiredPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    int timeoutMs = getPeerTimeoutSeconds() * 1000;
    
    auto it = peers.begin();
    while (it != peers.end()) {
        if (now - it->second.lastHeartbeat > timeoutMs) {
            // Save to lost peers list
            LostPeerInfo lost;
            lost.peer = it->second;
            lost.lostTime = now;
            
            lostPeers.push_back(lost);
            
            // Trim if too many
            if (lostPeers.size() > MAX_LOST_PEERS) {
                lostPeers.pop_front();
            }
            
//            std::cout << "Peer " << it->first << " (" << it->second.name 
//                      << ") timed out" << std::endl;
            
            it = peers.erase(it);
        } else {
            ++it;
        }
    }
    
    // Clean up old lost peers (older than retention time)
    int retentionMs = LOST_PEER_RETENTION_MINUTES * 60 * 1000;
    while (!lostPeers.empty() && 
           (now - lostPeers.front().lostTime) > retentionMs) {
        lostPeers.pop_front();
    }
}

std::string MeshManager::getLostPeersJSON() {
    std::lock_guard<std::mutex> lock(peersMutex);
    
    json_t* result = json_object();
    json_t* lost_array = json_array();
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    for (const auto& lost : lostPeers) {
        json_t* lostObj = json_object();
        
        // Include peer info
        json_object_set_new(lostObj, "applianceId", 
            json_string(lost.peer.applianceId.c_str()));
        json_object_set_new(lostObj, "name", 
            json_string(lost.peer.name.c_str()));
        json_object_set_new(lostObj, "lastStatus", 
            json_string(lost.peer.status.c_str()));
        json_object_set_new(lostObj, "lastIpAddress", 
            json_string(lost.peer.ipAddress.c_str()));
        
        // Add timing info
        json_object_set_new(lostObj, "lostTime", json_integer(lost.lostTime));
        
        // Add human-readable time ago
        int secondsAgo = (now - lost.lostTime) / 1000;
        std::string timeAgo;
        if (secondsAgo < 60) {
            timeAgo = std::to_string(secondsAgo) + " seconds ago";
        } else if (secondsAgo < 3600) {
            timeAgo = std::to_string(secondsAgo / 60) + " minutes ago";
        } else {
            timeAgo = std::to_string(secondsAgo / 3600) + " hours ago";
        }
        json_object_set_new(lostObj, "timeAgo", json_string(timeAgo.c_str()));
        
        json_array_append_new(lost_array, lostObj);
    }
    
    json_object_set_new(result, "lostPeers", lost_array);
    json_object_set_new(result, "count", json_integer(lostPeers.size()));
    
    char* resultStr = json_dumps(result, 0);
    std::string returnValue(resultStr);
    free(resultStr);
    json_decref(result);
    
    return returnValue;
}

void MeshManager::updateStatus(const std::string& status) {
    if (myStatus != status) {
        myStatus = status;
    }
}

void MeshManager::setCustomField(const std::string& key, const std::string& value) 
{
    {
        std::lock_guard<std::mutex> lock(customFieldsMutex);
        
        if (key.length() > 64 || customFields.size() >= 20) {
            std::cerr << "Custom field rejected: " << key << " (limits exceeded)" << std::endl;
            return;
        }
        
        auto it = customFields.find(key);
        if (it != customFields.end() && it->second == value) {
            return;  // No change needed
        }
        
        customFields[key] = value;
    }
}

void MeshManager::removeCustomField(const std::string& key) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(customFieldsMutex);
        removed = (customFields.erase(key) > 0);
    }
}

void MeshManager::clearCustomFields() {
    {
        std::lock_guard<std::mutex> lock(customFieldsMutex);
        customFields.clear();
    }
}

std::map<std::string, std::string> MeshManager::getCustomFields() {
    std::lock_guard<std::mutex> lock(customFieldsMutex);
    return customFields;  // Return copy
}


std::vector<MeshManager::PeerInfo> MeshManager::getPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<PeerInfo> peerList;
    
    for (const auto& [id, peer] : peers) {
        peerList.push_back(peer);
    }
    
    return peerList;
}

std::string MeshManager::getLocalIPAddress() {
    struct ifaddrs *ifap, *ifa;
    char ip[INET_ADDRSTRLEN];
    
    if (getifaddrs(&ifap) == 0) {
        for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;  // Skip loopback
            if (!(ifa->ifa_flags & IFF_UP)) continue;     // Skip down interfaces
            
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &addr->sin_addr, ip, INET_ADDRSTRLEN)) {
                freeifaddrs(ifap);
                return std::string(ip);
            }
        }
        freeifaddrs(ifap);
    }
    
    // Fallback - this shouldn't happen in normal operation
    return "127.0.0.1";
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
    json_object_set_new(self, "ipAddress", json_string(getLocalIPAddress().c_str()));
	json_object_set_new(self, "webPort", json_integer(httpPort));
    json_object_set_new(self, "isLocal", json_true());
    
    // Add all custom fields
    {
        std::lock_guard<std::mutex> customLock(customFieldsMutex);
        for (const auto& [key, value] : customFields) {
            json_object_set_new(self, key.c_str(), json_string(value.c_str()));
        }
    }
    
    json_array_append_new(appliances_array, self);
    
    // Add peers
    for (const auto& [id, peer] : peers) {
        json_t* peerObj = json_object();
        
        // Core fields
        json_object_set_new(peerObj, "applianceId", json_string(peer.applianceId.c_str()));
        json_object_set_new(peerObj, "name", json_string(peer.name.c_str()));
        json_object_set_new(peerObj, "status", json_string(peer.status.c_str()));
        json_object_set_new(peerObj, "ipAddress", json_string(peer.ipAddress.c_str()));
        json_object_set_new(peerObj, "webPort", json_integer(peer.webPort));
        json_object_set_new(peerObj, "isLocal", json_false());
        
        // Add all custom fields
        for (const auto& [fieldKey, fieldValue] : peer.customFields) {
            json_object_set_new(peerObj, fieldKey.c_str(), json_string(fieldValue.c_str()));
        }
        
        json_array_append_new(appliances_array, peerObj);
    }
    
    json_object_set_new(result, "appliances", appliances_array);
    
	if (!lostPeers.empty()) {
		json_t* lost_array = json_array();
		auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		
		for (const auto& lost : lostPeers) {
			json_t* lostObj = json_object();
			
			// Include all the same fields as getLostPeersJSON()
			json_object_set_new(lostObj, "applianceId", 
				json_string(lost.peer.applianceId.c_str()));
			json_object_set_new(lostObj, "name", 
				json_string(lost.peer.name.c_str()));
			json_object_set_new(lostObj, "lastStatus", 
				json_string(lost.peer.status.c_str()));
			json_object_set_new(lostObj, "lastIpAddress", 
				json_string(lost.peer.ipAddress.c_str()));
			json_object_set_new(lostObj, "lostTime", json_integer(lost.lostTime));
			
			// Add human-readable time ago
			int secondsAgo = (now - lost.lostTime) / 1000;
			std::string timeAgo;
			if (secondsAgo < 60) {
				timeAgo = std::to_string(secondsAgo) + " seconds ago";
			} else if (secondsAgo < 3600) {
				timeAgo = std::to_string(secondsAgo / 60) + " minutes ago";
			} else {
				timeAgo = std::to_string(secondsAgo / 3600) + " hours ago";
			}
			json_object_set_new(lostObj, "timeAgo", json_string(timeAgo.c_str()));
			
			json_array_append_new(lost_array, lostObj);
		}
		json_object_set_new(result, "recentlyLost", lost_array);
	}
    
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
    } else if (path == "/api/lost-peers") {
		response = getLostPeersJSON();
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

void MeshManager::addMeshSubscriber(uWS::WebSocket<false, true, MeshWSData>* ws) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    meshSubscribers.insert(ws);
    //    std::cout << "Added mesh WebSocket subscriber (total: " << meshSubscribers.size() << ")" << std::endl;
}

void MeshManager::removeMeshSubscriber(uWS::WebSocket<false, true, MeshWSData>* ws) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    meshSubscribers.erase(ws);
    //    std::cout << "Removed mesh WebSocket subscriber (total: " << meshSubscribers.size() << ")" << std::endl;
}

void MeshManager::broadcastMeshUpdate() {
    // Prepare the message
    json_t* update = json_object();
    json_object_set_new(update, "type", json_string("mesh_update"));
    json_object_set_new(update, "data", json_loads(getPeersJSON().c_str(), 0, nullptr));
    
    char* message = json_dumps(update, 0);
    std::string messageStr(message);
    free(message);
    json_decref(update);
    
    // Send directly to mesh subscribers (no deferring needed)
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
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
}

void MeshManager::broadcastCustomUpdate(const std::string& standardJson, const std::string& customJson) {
    // Similar pattern - direct sending, no deferring
    json_t* update = json_object();
    json_object_set_new(update, "type", json_string("mesh_custom_update"));
    
    json_error_t error;
    json_t* standardData = json_loads(standardJson.c_str(), 0, &error);
    json_t* customData = json_loads(customJson.c_str(), 0, &error);
    
    if (standardData) json_object_set_new(update, "standardData", standardData);
    if (customData) json_object_set_new(update, "customData", customData);
    
    char* message = json_dumps(update, 0);
    std::string messageStr(message);
    free(message);
    json_decref(update);
    
    // Send directly to mesh subscribers
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
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
}

// Add custom field management commands

// Mesh manager commands available to all interpreters

int MeshManager::mesh_update_status_command(ClientData clientData, Tcl_Interp* interp, 
                                      int objc, Tcl_Obj* const objv[]) {
   MeshManager* mesh = static_cast<MeshManager*>(clientData);
     
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "status");
        return TCL_ERROR;
    }
    
    mesh->updateStatus(Tcl_GetString(objv[1]));
    return TCL_OK;
}

int MeshManager::mesh_set_field_command(ClientData clientData, Tcl_Interp* interp, 
                                  int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "field value");
        return TCL_ERROR;
    }
    
    std::string field = Tcl_GetString(objv[1]);
    std::string value = Tcl_GetString(objv[2]);
    
    // Validate field name (no spaces, reasonable length)
    if (field.empty() || field.length() > 64 || 
        field.find(' ') != std::string::npos) {
        Tcl_AppendResult(interp, "Invalid field name: ", field.c_str(), NULL);
        return TCL_ERROR;
    }
    
    mesh->setCustomField(field, value);
    return TCL_OK;
}

int MeshManager::mesh_remove_field_command(ClientData clientData, Tcl_Interp* interp, 
                                     int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "field");
        return TCL_ERROR;
    }
    
    mesh->removeCustomField(Tcl_GetString(objv[1]));
    return TCL_OK;
}

int MeshManager::mesh_get_fields_command(ClientData clientData, Tcl_Interp* interp, 
                                   int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    auto fields = mesh->getCustomFields();
    Tcl_Obj* dict = Tcl_NewDictObj();
    
    for (const auto& [key, value] : fields) {
        Tcl_DictObjPut(interp, dict,
            Tcl_NewStringObj(key.c_str(), -1),
            Tcl_NewStringObj(value.c_str(), -1));
    }
    
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

int MeshManager::mesh_clear_fields_command(ClientData clientData, Tcl_Interp* interp, 
                                     int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    mesh->clearCustomFields();
    return TCL_OK;
}

int MeshManager::mesh_get_peers_command(ClientData clientData, Tcl_Interp* interp, 
                                  int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    std::vector<MeshManager::PeerInfo> peers = mesh->getPeers();
    
    Tcl_Obj* peerList = Tcl_NewListObj(0, nullptr);
    
    // Add each peer as a dictionary
    for (const auto& peer : peers) {
        Tcl_Obj* peerDict = Tcl_NewDictObj();
        
        // Core fields
        Tcl_DictObjPut(interp, peerDict, 
            Tcl_NewStringObj("id", -1), 
            Tcl_NewStringObj(peer.applianceId.c_str(), -1));
        Tcl_DictObjPut(interp, peerDict, 
            Tcl_NewStringObj("name", -1), 
            Tcl_NewStringObj(peer.name.c_str(), -1));
        Tcl_DictObjPut(interp, peerDict, 
            Tcl_NewStringObj("status", -1), 
            Tcl_NewStringObj(peer.status.c_str(), -1));
        Tcl_DictObjPut(interp, peerDict, 
            Tcl_NewStringObj("ip", -1), 
            Tcl_NewStringObj(peer.ipAddress.c_str(), -1));
        Tcl_DictObjPut(interp, peerDict, 
            Tcl_NewStringObj("webPort", -1), 
            Tcl_NewIntObj(peer.webPort));
        
        // Add all custom fields
        for (const auto& [key, value] : peer.customFields) {
            Tcl_DictObjPut(interp, peerDict, 
                Tcl_NewStringObj(key.c_str(), -1), 
                Tcl_NewStringObj(value.c_str(), -1));
        }
        
        Tcl_ListObjAppendElement(interp, peerList, peerDict);
    }
    
    // Also add self as a peer entry
    Tcl_Obj* selfDict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, selfDict, 
        Tcl_NewStringObj("id", -1), 
        Tcl_NewStringObj(mesh->getApplianceId().c_str(), -1));
    Tcl_DictObjPut(interp, selfDict, 
        Tcl_NewStringObj("name", -1), 
        Tcl_NewStringObj(mesh->getName().c_str(), -1));  // Need to add getName() method
    Tcl_DictObjPut(interp, selfDict, 
        Tcl_NewStringObj("status", -1), 
        Tcl_NewStringObj(mesh->getStatus().c_str(), -1));  // Need to add getStatus() method
    Tcl_DictObjPut(interp, selfDict, 
        Tcl_NewStringObj("ip", -1), 
        Tcl_NewStringObj("local", -1));
    Tcl_DictObjPut(interp, selfDict, 
        Tcl_NewStringObj("isLocal", -1), 
        Tcl_NewBooleanObj(1));
    
    // Add our own custom fields
    auto customFields = mesh->getCustomFields();
    for (const auto& [key, value] : customFields) {
        Tcl_DictObjPut(interp, selfDict, 
            Tcl_NewStringObj(key.c_str(), -1), 
            Tcl_NewStringObj(value.c_str(), -1));
    }
    
    Tcl_ListObjAppendElement(interp, peerList, selfDict);
    
    Tcl_SetObjResult(interp, peerList);
    return TCL_OK;
}

int MeshManager::mesh_get_cluster_status_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    return mesh_get_peers_command(clientData, interp, objc, objv);
}

int MeshManager::mesh_get_appliance_id_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(mesh->getApplianceId().c_str(), -1));
    return TCL_OK;
}

int MeshManager::mesh_broadcast_custom_update_command(ClientData clientData, Tcl_Interp *interp, 
	int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    std::string standardJson = Tcl_GetString(objv[1]);
    std::string customJson = Tcl_GetString(objv[2]);
    
    mesh->broadcastCustomUpdate(standardJson, customJson);
    
    return TCL_OK;
}

int MeshManager::mesh_config_command(ClientData clientData, Tcl_Interp* interp, 
                               int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?value?");
        return TCL_ERROR;
    }
    
    const char* option = Tcl_GetString(objv[1]);
    
    // GET operations (2 arguments)
    if (objc == 2) {
        if (strcmp(option, "heartbeatInterval") == 0) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(mesh->getHeartbeatInterval()));
            return TCL_OK;
        }
        else if (strcmp(option, "peerTimeout") == 0) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(mesh->getPeerTimeoutSeconds()));
            return TCL_OK;
        }
        else if (strcmp(option, "timeoutMultiplier") == 0) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(mesh->getPeerTimeoutMultiplier()));
            return TCL_OK;
        }
        else {
            Tcl_AppendResult(interp, "unknown option \"", option, 
                           "\", must be heartbeatInterval, peerTimeout, or timeoutMultiplier", NULL);
            return TCL_ERROR;
        }
    }
    
    // SET operations (3 arguments)
    if (objc == 3) {
        int value;
        if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK) {
            return TCL_ERROR;
        }
        
        if (strcmp(option, "heartbeatInterval") == 0) {
            if (value < 1 || value > 300) {
                Tcl_AppendResult(interp, "heartbeatInterval must be between 1 and 300 seconds", NULL);
                return TCL_ERROR;
            }
            mesh->setHeartbeatInterval(value);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(value));
            return TCL_OK;
        }
        else if (strcmp(option, "timeoutMultiplier") == 0) {
            if (value < 2 || value > 20) {
                Tcl_AppendResult(interp, "timeoutMultiplier must be between 2 and 20", NULL);
                return TCL_ERROR;
            }
            mesh->setPeerTimeoutMultiplier(value);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(mesh->getPeerTimeoutSeconds()));
            return TCL_OK;
        }
        else {
            Tcl_AppendResult(interp, "cannot set \"", option, "\"", NULL);
            return TCL_ERROR;
        }
    }
    
    Tcl_WrongNumArgs(interp, 1, objv, "option ?value?");
    return TCL_ERROR;
}

// Convenience command to get all mesh settings
int MeshManager::mesh_info_command(ClientData clientData, Tcl_Interp* interp, 
                             int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    Tcl_Obj* dict = Tcl_NewDictObj();
    
    Tcl_DictObjPut(interp, dict, 
        Tcl_NewStringObj("heartbeatInterval", -1),
        Tcl_NewIntObj(mesh->getHeartbeatInterval()));
    
    Tcl_DictObjPut(interp, dict, 
        Tcl_NewStringObj("timeoutMultiplier", -1),
        Tcl_NewIntObj(mesh->getPeerTimeoutMultiplier()));
    
    Tcl_DictObjPut(interp, dict, 
        Tcl_NewStringObj("peerTimeout", -1),
        Tcl_NewIntObj(mesh->getPeerTimeoutSeconds()));
    
    Tcl_DictObjPut(interp, dict, 
        Tcl_NewStringObj("applianceId", -1),
        Tcl_NewStringObj(mesh->getApplianceId().c_str(), -1));
    
    Tcl_DictObjPut(interp, dict, 
        Tcl_NewStringObj("peerCount", -1),
        Tcl_NewIntObj(mesh->getPeers().size()));
    
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

int MeshManager::mesh_get_lost_peers_command(ClientData clientData, 
                                             Tcl_Interp* interp, 
                                             int objc, Tcl_Obj* const objv[]) {
    MeshManager* mesh = static_cast<MeshManager*>(clientData);
    
    Tcl_Obj* lostList = Tcl_NewListObj(0, nullptr);
    
    for (const auto& lost : mesh->lostPeers) {
        Tcl_Obj* lostDict = Tcl_NewDictObj();
        
        Tcl_DictObjPut(interp, lostDict,
            Tcl_NewStringObj("id", -1),
            Tcl_NewStringObj(lost.peer.applianceId.c_str(), -1));
        Tcl_DictObjPut(interp, lostDict,
            Tcl_NewStringObj("name", -1),
            Tcl_NewStringObj(lost.peer.name.c_str(), -1));
        // Add other fields as needed
        
        Tcl_ListObjAppendElement(interp, lostList, lostDict);
    }
    
    Tcl_SetObjResult(interp, lostList);
    return TCL_OK;
}

void MeshManager::addTclCommands(Tcl_Interp* interp)
{
    if (!interp) {
        std::cerr << "Cannot add mesh commands: null interpreter" << std::endl;
        return;
    }
    
    /* Register mesh-specific commands */
    Tcl_CreateObjCommand(interp, "meshGetPeers", mesh_get_peers_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshGetClusterStatus", mesh_get_cluster_status_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshUpdateStatus", mesh_update_status_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshGetApplianceId", mesh_get_appliance_id_command, this, nullptr);                       
    Tcl_CreateObjCommand(interp, "meshBroadcastCustomUpdate", mesh_broadcast_custom_update_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshConfig", mesh_config_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshInfo", mesh_info_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshSetField", mesh_set_field_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshRemoveField", mesh_remove_field_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshGetFields", mesh_get_fields_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshClearFields", mesh_clear_fields_command, this, nullptr);
    Tcl_CreateObjCommand(interp, "meshGetLostPeers", mesh_get_lost_peers_command, this, nullptr);

    
    std::cout << "Mesh Tcl commands registered successfully" << std::endl;
}


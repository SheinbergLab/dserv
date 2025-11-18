#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <condition_variable>
#include <set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __linux__
    #include <poll.h>
#endif

#include <tcl.h>
#include <jansson.h>

#include "TclServer.h"
#include "ObjectRegistry.h"
#include <tcl.h>

#include <App.h> 

// Forward declarations
class Dataserver;
class TclServer;

namespace uWS {
    template<bool SSL, bool isServer, typename USERDATA>
    struct WebSocket;
}

struct MeshWSData {
    SharedQueue<std::string>* rqueue;
    std::string client_name;
    std::vector<std::string> subscriptions;
    SharedQueue<client_request_t>* notification_queue;
    std::string dataserver_client_id;
};

struct MeshSubscriber {
    void* ws_ptr;                                       // For removal
    std::function<bool(const std::string&)> send_func;  // Type-erased send
};

class MeshManager {
public:
struct PeerInfo {
    // Core fields
    std::string applianceId;
    std::string name;
    std::string status;
    std::string ipAddress;
    int webPort = 0;
    long long lastHeartbeat = 0;
    bool ssl = false;
    // ALL other fields stored generically
    std::map<std::string, std::string> customFields;
};
private:
    Dataserver* ds;
    
    std::unique_ptr<TclServer> mesh_tclserver;
    std::thread mesh_ws_thread;
    int mesh_websocket_port;
    
    uWS::Loop *mesh_ws_loop = nullptr;  
    std::mutex mesh_ws_connections_mutex;
    std::map<std::string, uWS::WebSocket<false, true, MeshWSData>*> mesh_ws_connections;
    us_listen_socket_t *mesh_listen_socket = nullptr;
    std::atomic<bool> ws_should_stop{false};
    
    // Configuration
    std::string myApplianceId;
    std::string myName;
    int httpPort;
    int discoveryPort;
    int guiPort;
    bool isSSLEnabled;
  
    // Heartbeat configuration with sensible defaults
    std::atomic<int> heartbeatInterval{1};        // seconds between heartbeats
    std::atomic<int> peerTimeoutMultiplier{6};    // heartbeats missed before timeout
    
    // Rate limiting for broadcasts
    std::chrono::steady_clock::time_point lastBroadcastTime;
    static constexpr auto MIN_BROADCAST_INTERVAL = std::chrono::milliseconds(100);
    
    // For interrupting the heartbeat thread when interval changes
    std::condition_variable heartbeatCV;
    std::mutex heartbeatMutex;
    std::atomic<bool> intervalChanged{false};
    
    // Current state (always included)
    std::string myStatus = "Stopped";

    // All other fields are custom
    std::mutex customFieldsMutex;
    std::map<std::string, std::string> customFields;
    
    // Peer management
    std::map<std::string, PeerInfo> peers;
    std::mutex peersMutex;

	struct LostPeerInfo {
        PeerInfo peer;
        long long lostTime;  // When we detected the loss
    };
    
    std::deque<LostPeerInfo> lostPeers;  // Recent lost peers
    static constexpr size_t MAX_LOST_PEERS = 20;  // Keep last 20
    static constexpr int LOST_PEER_RETENTION_MINUTES = 30;  // Keep for 30 mins
        
    // Network components
    int udpSocket;
    int httpSocket;
    struct sockaddr_in broadcastAddr;
    
    // Threading
    std::atomic<bool> running{false};
    std::thread heartbeatThread;
    std::thread discoveryThread;
    std::thread httpThread;
    
    std::mutex subscribersMutex;

    // Network interface caching
    std::vector<std::string> cachedBroadcastAddresses;
    std::chrono::steady_clock::time_point lastNetworkScan;
    static constexpr std::chrono::seconds NETWORK_SCAN_INTERVAL{30}; // Refresh every 30 seconds
    std::mutex broadcastCacheMutex;
    
    // Methods
    bool joinThreadWithTimeout(std::thread& t, std::chrono::seconds timeout); 
    
    std::vector<std::string> scanNetworkBroadcastAddresses();
    std::vector<std::string> getBroadcastAddresses();
    void refreshBroadcastCache();
    void triggerBroadcast();
    void handleMeshWebSocketMessage(auto *ws, std::string_view message);
    void addTclCommands();
    void notifyWebSocketClients();
    
    // passed from main
    int argc;
    char **argv;
public:
    MeshManager(Dataserver* ds, int argc, char *argv[], 
		int http_port = 12348, int discovery_port = 12346, int websocket_port = 2569,
		int gui_port = 2565,
		bool ssl_enabled = false);

    ~MeshManager();

    // factory for our manager    
  static std::unique_ptr<MeshManager> createAndStart(
						   Dataserver* ds, TclServer* main_tclserver, int argc, char** argv,
						   const std::string& appliance_id = "",
						   const std::string& appliance_name = "",
						   int http_port = 12348, int discovery_port = 12346,
						   int websocket_port = 2569,
						   int gui_port = 2565);


    // Configuration and startup
	void init(const std::string& applianceId, const std::string& name, 
	          int mesh_tcl_port = 2575);
    void setHttpPort(int port) { httpPort = port; }
    void setDiscoveryPort(int port) { discoveryPort = port; }
    void setSSLEnabbled(bool status) { isSSLEnabled = status; }
  
    void start();
    void stop();
    
    // Tcl support
    void addTclCommands(Tcl_Interp* interp);
    void startMeshWebSocketServer(int port);
    void setWebSocketPort(int port) { mesh_websocket_port = port; }
    
    // Status management (called from Tcl)
    void updateStatus(const std::string& status);

    // Custom field management
    void setCustomField(const std::string& key, const std::string& value);
    void removeCustomField(const std::string& key);
    void clearCustomFields();
    std::map<std::string, std::string> getCustomFields();
    
    // Getters for Tcl
    std::string getApplianceId() const { return myApplianceId; }
    std::string getName() const { return myName; }
    std::string getStatus() const { return myStatus; }
    std::vector<PeerInfo> getPeers();
    std::string getPeersJSON();
    std::string getLostPeersJSON();
    
    // Register Tcl commands with the main interpreter
    void registerTclCommands();

    std::vector<MeshSubscriber> meshSubscribers;  

    void broadcastMeshUpdate();
    void broadcastCustomUpdate(const std::string& standardJson, const std::string& customJson);
    
    // Configuration methods
    void setHeartbeatInterval(int seconds);
    int getHeartbeatInterval() const { return heartbeatInterval.load(); }
    
    void setPeerTimeoutMultiplier(int multiplier);
    int getPeerTimeoutMultiplier() const { return peerTimeoutMultiplier.load(); }
    
    // Get computed timeout (interval * multiplier)
    int getPeerTimeoutSeconds() const { 
        return heartbeatInterval.load() * peerTimeoutMultiplier.load(); 
    }

    // Make these inline templates in the header
  template<typename WebSocketType>
  void addMeshSubscriber(WebSocketType* ws) {
    std::lock_guard<std::mutex> lock(subscribersMutex);
    
    MeshSubscriber sub;
    sub.ws_ptr = (void*)ws;
    sub.send_func = [ws](const std::string& msg) -> bool {  // Add explicit -> bool
      try {
	auto result = ws->send(msg, uWS::OpCode::TEXT);
	// SendStatus is typically truthy/falsy, so convert to bool
	return static_cast<bool>(result);
      } catch (...) {
	return false;
      }
    };
    
    meshSubscribers.push_back(sub);
  }
    
    template<typename WebSocketType>
    void removeMeshSubscriber(WebSocketType* ws) {
        std::lock_guard<std::mutex> lock(subscribersMutex);
        void* ws_ptr = (void*)ws;
        
        meshSubscribers.erase(
            std::remove_if(meshSubscribers.begin(), meshSubscribers.end(),
                [ws_ptr](const MeshSubscriber& sub) {
                    return sub.ws_ptr == ws_ptr;
                }),
            meshSubscribers.end()
        );
    }
    
private:
    // Network setup and management
    void setupUDP();
    void setupHTTP();
    void sendHeartbeat();
    void listenForHeartbeats();
    void updatePeer(json_t* heartbeat, const std::string& ip);
    void cleanupExpiredPeers();
    std::string getLocalIPAddress();
    static std::string getHostname();

    // HTTP API server
    void runHttpServer();
    void handleHttpRequest(int clientSocket);
    std::string getMeshHTML();
    
    // Tcl command implementations (static callbacks)
	static int mesh_get_peers_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_get_cluster_status_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_update_status_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_get_appliance_id_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_broadcast_custom_update_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_config_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_info_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_set_field_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_remove_field_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_get_fields_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_clear_fields_command(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
	static int mesh_get_lost_peers_command(ClientData clientData, 
                                             Tcl_Interp* interp, 
                                             int objc, Tcl_Obj* const objv[]);	
};

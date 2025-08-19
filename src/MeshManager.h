#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <tcl.h>
#include <jansson.h>

// Forward declarations
class Dataserver;
class TclServer;

namespace uWS {
    template<bool SSL, bool isServer, typename USERDATA>
    struct WebSocket;
}

struct WSPerSocketData; // Forward declare

class MeshManager {
public:
    struct PeerInfo {
        std::string applianceId;
        std::string name;
        std::string status;
        std::string currentExperiment;
        std::string participantId;
        long long lastHeartbeat;
        std::string ipAddress;
        int webPort;
    };

private:
    Dataserver* ds;
    TclServer* tclserver;  // Access to main TclServer instead of own interp
    
    // Configuration
    std::string myApplianceId;
    std::string myName;
    int httpPort;
    int discoveryPort;
    
    // Current state
    std::string myStatus;
    std::string currentExperiment;
    std::string participantId;
    
    // Peer management
    std::map<std::string, PeerInfo> peers;
    std::mutex peersMutex;
    
    // Network components
    int udpSocket;
    int httpSocket;
    struct sockaddr_in broadcastAddr;
    
    // Threading
    std::atomic<bool> running{true};
    std::thread heartbeatThread;
    std::thread discoveryThread;
    std::thread httpThread;
    
    std::set<uWS::WebSocket<false, true, WSPerSocketData>*> meshSubscribers;
    std::mutex subscribersMutex;

public:
    MeshManager(Dataserver* ds, TclServer* tclserver);
    ~MeshManager();
    
    // Configuration and startup
    void init(const std::string& applianceId, const std::string& name);
    void setHttpPort(int port) { httpPort = port; }
    void setDiscoveryPort(int port) { discoveryPort = port; }
    void start();
    void stop();
    
    // Status management (called from Tcl)
    void updateStatus(const std::string& status);
    void updateExperiment(const std::string& experiment);
    void updateParticipant(const std::string& participant);
    
    // Getters for Tcl
    std::string getApplianceId() const { return myApplianceId; }
    std::string getName() const { return myName; }
    std::string getStatus() const { return myStatus; }
    std::vector<PeerInfo> getPeers();
    std::string getPeersJSON();
    
    // Register Tcl commands with the main interpreter
    void registerTclCommands();

    void addMeshSubscriber(uWS::WebSocket<false, true, WSPerSocketData>* ws);
    void removeMeshSubscriber(uWS::WebSocket<false, true, WSPerSocketData>* ws);
    void broadcastMeshUpdate();
    void broadcastCustomUpdate(const std::string& standardJson, const std::string& customJson);
    
private:
    // Network setup and management
    void setupUDP();
    void setupHTTP();
    void sendHeartbeat();
    void listenForHeartbeats();
    void updatePeer(json_t* heartbeat, const std::string& ip);
    void cleanupExpiredPeers();
    
    // HTTP API server
    void runHttpServer();
    void handleHttpRequest(int clientSocket);
    std::string getMeshHTML();
    
    // Tcl command implementations (static callbacks)
    static int meshSetStatusCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshUpdateStatusCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshUpdateExperimentCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshUpdateParticipantCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshGetPeersCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshGetClusterStatusCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int meshGetApplianceIdCommand(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
};
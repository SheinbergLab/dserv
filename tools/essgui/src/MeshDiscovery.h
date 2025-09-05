#ifndef MESH_DISCOVERY_H
#define MESH_DISCOVERY_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <cstdint>

class MeshDiscovery {
public:
    struct PeerInfo {
        std::string applianceId;
        std::string name;
        std::string status;
        std::string ipAddress;
        int webPort = 0;
        int64_t lastSeen = 0;
        std::map<std::string, std::string> customFields;
        
        // Helper to create display text for UI
        std::string getDisplayText() const {
            if (!name.empty() && name != ipAddress) {
                return name + " (" + ipAddress + ")";
            }
            return ipAddress;
        }
        
        // Helper to check if this peer has dserv capability
        bool hasDataserver() const {
            return customFields.find("dserv_port") != customFields.end() ||
                   customFields.find("system") != customFields.end();
        }
    };
    
    explicit MeshDiscovery(int discoveryPort = 12346);
    ~MeshDiscovery();
    
    // Main discovery interface
    int discoverPeers(int timeoutMs = 2000);
    
    // Data access
    std::vector<PeerInfo> getPeers() const;
    std::vector<std::string> getPeerAddresses() const;
    std::vector<std::string> getPeerDisplayTexts() const;
    size_t getPeerCount() const;
    
    // Utility functions
    bool isLocalhostAvailable() const;
    void cleanupExpiredPeers();
    void clearPeers();
    
    // Real-time discovery callback
    void setDiscoveryCallback(std::function<void(const PeerInfo&)> callback);
    
    // Extract IP from display text "Name (IP)" format
    static std::string extractIpFromDisplayText(const std::string& displayText);
     int createSocket();
        
private:
    void processMeshHeartbeat(const char* data, const char* senderIP);

    void closeSocket();
    bool testLocalhostConnection() const;
    
    int discoveryPort;
    int meshSocket;
    mutable std::mutex peersMutex;
    std::map<std::string, PeerInfo> discoveredPeers;
    std::function<void(const PeerInfo&)> discoveryCallback;
    
    static const int PEER_TIMEOUT_MS = 30000;  // 30 seconds
    static const int LOCALHOST_TEST_PORT = 4620;  // Default dserv port
};

#endif // MESH_DISCOVERY_H
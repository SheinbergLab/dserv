#pragma once

#include <QWidget>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QProgressBar>
#include <QToolButton>
#include <QUdpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QHostAddress>

class EssCommandInterface;

struct MeshPeerInfo {
    QString applianceId;
    QString name;
    QString status;
    QString ipAddress;
    int webPort = 0;
    bool isLocal = false;
    qint64 lastSeen = 0;
    QJsonObject customFields;
    
    // Helper to check if this is a lab system (has dserv running)
    bool isLabSystem() const {
        return customFields.contains("dserv_port") || webPort > 0;
    }
};

class EssHostDiscoveryWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssHostDiscoveryWidget(QWidget *parent = nullptr);
    ~EssHostDiscoveryWidget();

    // Public interface
    QString currentHost() const;
    QStringList discoveredHosts() const;
    bool isRefreshing() const { return m_isRefreshing; }

public slots:
    void refreshHosts();
    void connectToSelected();
    void disconnectFromHost();
    void updateConnectionStatus();
    bool isLocalhostRunning();

signals:
    void hostSelected(const QString &host);
    void refreshStarted();
    void refreshFinished();
    void connectionStateChanged(bool connected, const QString &host);

private slots:
    void onHostSelectionChanged(int index);
    void onRefreshTimeout();
    void onMeshHeartbeatReceived();
    void processMeshHeartbeat(const QByteArray &data, const QHostAddress &senderAddress);
    
    // Command interface connection signals
    void onConnected(const QString &host);
    void onDisconnected();
    void onConnectionError(const QString &error);
    
private:
    void setupUi();
    void connectSignals();
    void updateUiState();
    void updateConnectionIndicator(bool connected);
    
    // Mesh discovery methods
    void startMeshDiscovery();
    void stopMeshDiscovery();
    void setupMeshSocket();
    void cleanupExpiredPeers();
    void updateHostsFromMeshPeers();
    
    // UI Components - ultra compact
    QComboBox *m_hostCombo;
    QToolButton *m_refreshButton;
    QToolButton *m_connectButton;
    QToolButton *m_disconnectButton;
    QLabel *m_connectionIndicator;  // Visual connection status
    QProgressBar *m_progressBar;
    
    // State
    QString m_connectedHost;
    QTimer *m_refreshTimer;
    bool m_isRefreshing;
    QString m_pendingConnectionHost;
    
    // Command interface reference
    EssCommandInterface *m_commandInterface;
    
    // Mesh discovery components
    QUdpSocket *m_meshSocket;
    QTimer *m_meshDiscoveryTimer;
    QTimer *m_peerCleanupTimer;
    QMap<QString, MeshPeerInfo> m_discoveredPeers;
    
    // Mesh discovery configuration
    static constexpr int MESH_DISCOVERY_PORT = 12346; // Your mesh discovery port
    static constexpr int PEER_TIMEOUT_MS = 30000;     // 30 seconds timeout
    static constexpr int CLEANUP_INTERVAL_MS = 10000; // Clean up every 10 seconds
    static constexpr int DISCOVERY_INTERVAL_MS = 2000; // Listen for 2 seconds during refresh (reduced from 5s)
};
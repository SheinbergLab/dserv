#include "EssHostDiscoveryWidget.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include "communication/DservClient.h"

#include <QMessageBox>
#include <QThread>
#include <QDateTime>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QJsonParseError>

EssHostDiscoveryWidget::EssHostDiscoveryWidget(QWidget *parent)
    : QWidget(parent)
    , m_isRefreshing(false)
    , m_commandInterface(nullptr)
    , m_meshSocket(nullptr)
{
    setupUi();
    connectSignals();
    
    // Setup mesh discovery
    setupMeshSocket();
    
    // Setup timers
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &EssHostDiscoveryWidget::onRefreshTimeout);
    
    // Mesh discovery timer - listens for heartbeats during refresh periods
    m_meshDiscoveryTimer = new QTimer(this);
    m_meshDiscoveryTimer->setSingleShot(true);
    
    // Peer cleanup timer - runs continuously to remove stale entries
    m_peerCleanupTimer = new QTimer(this);
    m_peerCleanupTimer->setInterval(CLEANUP_INTERVAL_MS);
    connect(m_peerCleanupTimer, &QTimer::timeout, this, &EssHostDiscoveryWidget::cleanupExpiredPeers);
    m_peerCleanupTimer->start();
    
    // Log initialization
    EssConsoleManager::instance()->logSystem("Host Discovery widget initialized with mesh discovery", "Discovery");
    
    // Auto-refresh on startup
    QTimer::singleShot(100, this, &EssHostDiscoveryWidget::refreshHosts);
}

EssHostDiscoveryWidget::~EssHostDiscoveryWidget()
{
    stopMeshDiscovery();
}

void EssHostDiscoveryWidget::setupMeshSocket()
{
    m_meshSocket = new QUdpSocket(this);
    
    // Bind to the mesh discovery port to listen for heartbeats
    if (m_meshSocket->bind(QHostAddress::Any, MESH_DISCOVERY_PORT, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        EssConsoleManager::instance()->logSuccess(
            QString("Mesh discovery listening on port %1").arg(MESH_DISCOVERY_PORT), 
            "Discovery"
        );
        
        connect(m_meshSocket, &QUdpSocket::readyRead, 
                this, &EssHostDiscoveryWidget::onMeshHeartbeatReceived);
    } else {
        EssConsoleManager::instance()->logError(
            QString("Failed to bind mesh discovery socket: %1").arg(m_meshSocket->errorString()), 
            "Discovery"
        );
        
        // Try to continue without mesh discovery
        m_meshSocket->deleteLater();
        m_meshSocket = nullptr;
    }
}

void EssHostDiscoveryWidget::setupUi()
{
    // Single horizontal layout - everything on one line
    auto *layout = new QHBoxLayout(this);
    layout->setSpacing(2);
    layout->setContentsMargins(0, 0, 10, 0);
    
    // Host selection combo
    m_hostCombo = new QComboBox();
    m_hostCombo->setMinimumWidth(120);
    m_hostCombo->setMaximumWidth(200);
    m_hostCombo->setToolTip("Select a host to connect");
    m_hostCombo->setPlaceholderText("Select host...");
    
    // Compact buttons
    m_refreshButton = new QToolButton();
    m_refreshButton->setText("↻");
    m_refreshButton->setToolTip("Refresh - Search for available lab systems via mesh heartbeats");
    m_refreshButton->setAutoRaise(true);
    
    m_connectButton = new QToolButton();
    m_connectButton->setText("→");
    m_connectButton->setEnabled(false);
    m_connectButton->setToolTip("Connect to selected host");
    m_connectButton->setAutoRaise(true);
    
    m_disconnectButton = new QToolButton();
    m_disconnectButton->setText("×");
    m_disconnectButton->setEnabled(false);
    m_disconnectButton->setToolTip("Disconnect from current host");
    m_disconnectButton->setAutoRaise(true);
    
    // Connection indicator - small colored circle
    m_connectionIndicator = new QLabel();
    m_connectionIndicator->setFixedSize(12, 12);
    m_connectionIndicator->setToolTip("Disconnected");
    updateConnectionIndicator(false);
    
    // Add all to layout
    layout->addWidget(m_hostCombo, 1);  // Combo box takes stretch space
    layout->addWidget(m_refreshButton);
    layout->addWidget(m_connectButton);
    layout->addWidget(m_disconnectButton);
    layout->addStretch();  // Push indicator to the right
    layout->addWidget(m_connectionIndicator);
    
    // Set fixed height for ultra-compact appearance
    setMaximumHeight(32);
}

void EssHostDiscoveryWidget::connectSignals()
{
    // Get command interface from application
    if (EssApplication::instance()) {
        m_commandInterface = EssApplication::instance()->commandInterface();
        if (m_commandInterface) {
            // Connect to command interface signals
            connect(m_commandInterface, &EssCommandInterface::connected,
                    this, &EssHostDiscoveryWidget::onConnected);
            connect(m_commandInterface, &EssCommandInterface::disconnected,
                    this, &EssHostDiscoveryWidget::onDisconnected);
            connect(m_commandInterface, &EssCommandInterface::connectionError,
                    this, &EssHostDiscoveryWidget::onConnectionError);
        }
        
        connect(EssApplication::instance(), &EssApplication::disconnectCancelled,
                this, [this]() {
                    EssConsoleManager::instance()->logInfo(
                        "Disconnect cancelled - keeping connection", 
                        "Discovery"
                    );
                });
    }
    
    // UI connections
    connect(m_refreshButton, &QToolButton::clicked, 
            this, &EssHostDiscoveryWidget::refreshHosts);
    connect(m_connectButton, &QToolButton::clicked, 
            this, &EssHostDiscoveryWidget::connectToSelected);
    connect(m_disconnectButton, &QToolButton::clicked, 
            this, &EssHostDiscoveryWidget::disconnectFromHost);
    
    // Combo box selection
    connect(m_hostCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssHostDiscoveryWidget::onHostSelectionChanged);
    
    // Double-click simulation on combo box
    connect(m_hostCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
                if (index >= 0 && m_connectedHost.isEmpty()) {
                    connectToSelected();
                }
            });
}

QString EssHostDiscoveryWidget::currentHost() const
{
    if (m_commandInterface) {
        return m_commandInterface->currentHost();
    }
    return m_connectedHost;
}

QStringList EssHostDiscoveryWidget::discoveredHosts() const
{
    QStringList hosts;
    for (int i = 0; i < m_hostCombo->count(); ++i) {
        hosts << m_hostCombo->itemText(i);
    }
    return hosts;
}

void EssHostDiscoveryWidget::refreshHosts()
{
    if (m_isRefreshing) {
        return;
    }
    
    m_isRefreshing = true;
    m_refreshButton->setEnabled(false);
    
    EssConsoleManager::instance()->logInfo("Starting mesh heartbeat discovery", "Discovery");
    
    emit refreshStarted();
    
    // Start mesh discovery for a limited time
    startMeshDiscovery();
}

void EssHostDiscoveryWidget::startMeshDiscovery()
{
    if (!m_meshSocket) {
        // No mesh socket available, finish immediately
        onRefreshTimeout();
        return;
    }
    
    // Listen for heartbeats for DISCOVERY_INTERVAL_MS
    m_meshDiscoveryTimer->start(DISCOVERY_INTERVAL_MS);
    connect(m_meshDiscoveryTimer, &QTimer::timeout, this, &EssHostDiscoveryWidget::onRefreshTimeout, Qt::UniqueConnection);
    
    EssConsoleManager::instance()->logDebug(
        QString("Listening for mesh heartbeats for %1 seconds").arg(DISCOVERY_INTERVAL_MS / 1000.0, 0, 'f', 1), 
        "Discovery"
    );
}

void EssHostDiscoveryWidget::stopMeshDiscovery()
{
    if (m_meshDiscoveryTimer) {
        m_meshDiscoveryTimer->stop();
    }
}

void EssHostDiscoveryWidget::onRefreshTimeout()
{
    m_isRefreshing = false;
    m_refreshButton->setEnabled(true);
    
    // Update the host combo with discovered peers
    updateHostsFromMeshPeers();
    
    // Update connected host highlighting
    updateConnectionStatus();
    
    emit refreshFinished();
}

void EssHostDiscoveryWidget::onMeshHeartbeatReceived()
{
    while (m_meshSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_meshSocket->pendingDatagramSize());
        
        QHostAddress sender;
        quint16 senderPort;
        
        if (m_meshSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort) > 0) {
            processMeshHeartbeat(datagram, sender);
        }
    }
}

void EssHostDiscoveryWidget::processMeshHeartbeat(const QByteArray &data, const QHostAddress &senderAddress)
{
    // Parse JSON heartbeat message
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        return; // Invalid JSON, ignore
    }
    
    QJsonObject heartbeat = doc.object();
    
    // Validate this is a heartbeat message
    if (heartbeat["type"].toString() != "heartbeat") {
        return;
    }
    
    QString applianceId = heartbeat["applianceId"].toString();
    if (applianceId.isEmpty()) {
        return;
    }
    
    QJsonObject heartbeatData = heartbeat["data"].toObject();
    
    // Create or update peer info
    MeshPeerInfo peer;
    peer.applianceId = applianceId;
    peer.name = heartbeatData["name"].toString();
    peer.status = heartbeatData["status"].toString();
    peer.ipAddress = senderAddress.toString();
    peer.webPort = heartbeatData["webPort"].toInt();
    peer.isLocal = false; // Remote peers only
    peer.lastSeen = QDateTime::currentMSecsSinceEpoch();
    
    // Store custom fields
    QJsonObject customFields;
    for (auto it = heartbeatData.begin(); it != heartbeatData.end(); ++it) {
        QString key = it.key();
        if (key != "name" && key != "status" && key != "webPort") {
            customFields[key] = it.value();
        }
    }
    peer.customFields = customFields;
    
    // Add to discovered peers
    m_discoveredPeers[applianceId] = peer;
    
    // Only log new peers or status changes, not every heartbeat
    static QMap<QString, QString> lastStatus;
    if (!lastStatus.contains(applianceId) || lastStatus[applianceId] != peer.status) {
        lastStatus[applianceId] = peer.status;
        EssConsoleManager::instance()->logDebug(
            QString("Mesh peer %1 (%2) at %3 - status: %4")
            .arg(peer.name)
            .arg(peer.applianceId)
            .arg(peer.ipAddress)
            .arg(peer.status), 
            "Discovery"
        );
    }
}

void EssHostDiscoveryWidget::cleanupExpiredPeers()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    
    auto it = m_discoveredPeers.begin();
    while (it != m_discoveredPeers.end()) {
        if (now - it->lastSeen > PEER_TIMEOUT_MS) {
            EssConsoleManager::instance()->logDebug(
                QString("Peer %1 (%2) timed out").arg(it->name).arg(it->applianceId), 
                "Discovery"
            );
            it = m_discoveredPeers.erase(it);
        } else {
            ++it;
        }
    }
    
    // Update UI if we removed any peers
    if (!m_isRefreshing) {
        updateHostsFromMeshPeers();
    }
}

void EssHostDiscoveryWidget::updateHostsFromMeshPeers()
{
    // Save current selection (now we need to extract IP from display text)
    QString currentSelection = m_hostCombo->currentText();
    QString currentIp;
    if (currentSelection.contains(" (") && currentSelection.endsWith(")")) {
        // Extract IP from "Name (IP)" format
        int start = currentSelection.lastIndexOf("(") + 1;
        int end = currentSelection.lastIndexOf(")");
        currentIp = currentSelection.mid(start, end - start);
    } else {
        currentIp = currentSelection; // Fallback for localhost or plain IP
    }
    
    // Track what we had before to avoid redundant logging
    static QStringList lastDiscoveredIps;
    static bool lastHadLocalhost = false;
    
    // Clear combo
    m_hostCombo->clear();
    
    QStringList addedIps; // Track IPs to avoid duplicates
    bool hasLocalhost = false;
    
    // Check if localhost should be added
    if (isLocalhostRunning()) {
        m_hostCombo->addItem("localhost");
        m_hostCombo->setItemData(m_hostCombo->count() - 1, "localhost", Qt::UserRole); // Store IP in UserRole
        addedIps.append("localhost");
        hasLocalhost = true;
        
        // Only log if localhost status changed
        if (!lastHadLocalhost) {
            EssConsoleManager::instance()->logInfo(
                "Added localhost (verified dserv is running)", 
                "Discovery"
            );
        }
    }
    
    // Add mesh-discovered systems - be much more permissive
    for (const MeshPeerInfo &peer : m_discoveredPeers) {
        // Clean up IPv6-mapped IPv4 addresses (::ffff:192.168.x.x -> 192.168.x.x)
        QString cleanIpAddress = peer.ipAddress;
        if (cleanIpAddress.startsWith("::ffff:")) {
            cleanIpAddress = cleanIpAddress.mid(7); // Remove "::ffff:" prefix
        }
        
        // Include any peer that's broadcasting heartbeats - they're part of the mesh
        // Skip localhost variants since we handle that separately
        if (!cleanIpAddress.isEmpty() && 
            cleanIpAddress != "127.0.0.1" && 
            cleanIpAddress != "localhost" &&
            !addedIps.contains(cleanIpAddress)) {
            
            // Create display text: "Name (IP)" or just "IP" if no name
            QString displayText;
            if (!peer.name.isEmpty() && peer.name != cleanIpAddress) {
                displayText = QString("%1 (%2)").arg(peer.name).arg(cleanIpAddress);
            } else {
                displayText = cleanIpAddress;
            }
            
            m_hostCombo->addItem(displayText);
            m_hostCombo->setItemData(m_hostCombo->count() - 1, cleanIpAddress, Qt::UserRole); // Store actual IP
            addedIps.append(cleanIpAddress);
            
            // Only log if this is a new discovery
            if (!lastDiscoveredIps.contains(cleanIpAddress)) {
                EssConsoleManager::instance()->logSuccess(
                    QString("Discovered mesh system: %1 (%2) at %3 - %4")
                    .arg(peer.name)
                    .arg(peer.applianceId)
                    .arg(cleanIpAddress)
                    .arg(peer.status), 
                    "Discovery"
                );
            }
        }
    }
    
    // Add hosts to combo box
    if (m_hostCombo->count() > 0) {
        // Try to restore previous selection by matching IP
        if (!m_connectedHost.isEmpty()) {
            // Find item by IP stored in UserRole
            for (int i = 0; i < m_hostCombo->count(); i++) {
                QString itemIp = m_hostCombo->itemData(i, Qt::UserRole).toString();
                if (itemIp == m_connectedHost) {
                    m_hostCombo->setCurrentIndex(i);
                    // Style the connected item
                    m_hostCombo->setItemData(i, QColor(87, 199, 135), Qt::ForegroundRole);
                    m_hostCombo->setItemData(i, QFont("", -1, QFont::Bold), Qt::FontRole);
                    break;
                }
            }
        } else if (!currentIp.isEmpty()) {
            // Try to restore previous selection by IP
            for (int i = 0; i < m_hostCombo->count(); i++) {
                QString itemIp = m_hostCombo->itemData(i, Qt::UserRole).toString();
                if (itemIp == currentIp) {
                    m_hostCombo->setCurrentIndex(i);
                    break;
                }
            }
        } else {
            // Not connected - set to placeholder state
            m_hostCombo->setCurrentIndex(-1);
        }
        
        // Only log summary if the list actually changed
        if (addedIps != lastDiscoveredIps || hasLocalhost != lastHadLocalhost) {
            EssConsoleManager::instance()->logSuccess(
                QString("Mesh discovery complete: found %1 system(s)").arg(m_hostCombo->count()), 
                "Discovery"
            );
        }
    } else {
        // Only log if we previously had systems but now have none
        if (!lastDiscoveredIps.isEmpty() || lastHadLocalhost) {
            m_hostCombo->setPlaceholderText("No mesh systems found");
            
            EssConsoleManager::instance()->logWarning(
                "No systems discovered via mesh heartbeats", 
                "Discovery"
            );
        }
    }
    
    // Update our tracking variables
    lastDiscoveredIps = addedIps;
    lastHadLocalhost = hasLocalhost;
}

void EssHostDiscoveryWidget::connectToSelected()
{
    QString selectedText = m_hostCombo->currentText();
    if (selectedText.isEmpty()) return;
    
    // Extract the actual IP address from the UserRole data
    QString host = m_hostCombo->currentData(Qt::UserRole).toString();
    if (host.isEmpty()) {
        // Fallback to the display text (for localhost or plain IP entries)
        host = selectedText;
    }
    
    if (!m_commandInterface) {
        EssConsoleManager::instance()->logError("Command interface not available", "Discovery");
        return;
    }
    
    EssConsoleManager::instance()->logInfo(
        QString("Connecting to host: %1").arg(host), 
        "Discovery"
    );
    
    // The actual connection is handled by EssCommandInterface
    bool success = m_commandInterface->connectToHost(host);
    
    if (!success) {
        EssConsoleManager::instance()->logError(
            QString("Failed to initiate connection to %1").arg(host), 
            "Discovery"
        );
    }
    
    emit hostSelected(host);
}

void EssHostDiscoveryWidget::disconnectFromHost()
{
    if (!m_commandInterface) {
        EssConsoleManager::instance()->logError("Command interface not available", "Discovery");
        return;
    }
    
    EssConsoleManager::instance()->logInfo("Requesting disconnect from host", "Discovery");
    
    // Request disconnect - the application will handle checking for unsaved changes
    m_commandInterface->requestDisconnect();
    
    // The actual UI update will happen in onDisconnected() slot when the disconnected signal is received
}

void EssHostDiscoveryWidget::onHostSelectionChanged(int index)
{
    bool hasSelection = index >= 0 && !m_hostCombo->currentText().isEmpty();
    QString currentHost = this->currentHost();
    
    m_connectButton->setEnabled(hasSelection && currentHost.isEmpty());
    
    // If user selects a different host while connected, offer to switch
    // Only do this if we actually have a valid selection and we're connected to something different
    if (hasSelection && !currentHost.isEmpty()) {
        QString selectedIp = m_hostCombo->currentData(Qt::UserRole).toString();
        if (selectedIp.isEmpty()) {
            selectedIp = m_hostCombo->currentText(); // Fallback for localhost
        }
        
        // Only ask if they're selecting a genuinely different host
        if (selectedIp != currentHost && !selectedIp.isEmpty()) {
            QString selectedDisplayName = m_hostCombo->currentText();
            
            int ret = QMessageBox::question(this, "Already Connected",
                                           QString("Already connected to %1. Disconnect and connect to %2?")
                                           .arg(currentHost).arg(selectedDisplayName),
                                           QMessageBox::Yes | QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                m_pendingConnectionHost = selectedIp;
                disconnectFromHost();
            } else {
                // Restore the connected host in the combo by finding it by IP
                for (int i = 0; i < m_hostCombo->count(); i++) {
                    QString itemIp = m_hostCombo->itemData(i, Qt::UserRole).toString();
                    if (itemIp.isEmpty()) itemIp = m_hostCombo->itemText(i); // Fallback
                    if (itemIp == currentHost) {
                        m_hostCombo->setCurrentIndex(i);
                        break;
                    }
                }
            }
        }
    }
}

void EssHostDiscoveryWidget::onConnected(const QString &host)
{
    m_connectedHost = host;
    updateUiState();
    
    // Update combo to show connected status
    int index = m_hostCombo->findText(host);
    if (index >= 0) {
        m_hostCombo->setCurrentIndex(index);
        m_hostCombo->setItemData(index, QColor(87, 199, 135), Qt::ForegroundRole);
        m_hostCombo->setItemData(index, QFont("", -1, QFont::Bold), Qt::FontRole);
    }
    
    updateConnectionIndicator(true);
    
    emit connectionStateChanged(true, host);
}

void EssHostDiscoveryWidget::onDisconnected()
{
    EssConsoleManager::instance()->logInfo("Disconnect signal received", "Discovery");
    
    QString oldHost = m_connectedHost;
    m_connectedHost.clear();
    
    // Clear styling from previously connected host
    if (!oldHost.isEmpty()) {
        int index = m_hostCombo->findText(oldHost);
        if (index >= 0) {
            m_hostCombo->setItemData(index, QVariant(), Qt::ForegroundRole);
            m_hostCombo->setItemData(index, QVariant(), Qt::FontRole);
        }
    }
    
    // Reset combo to show placeholder
    m_hostCombo->setCurrentIndex(-1);
    
    updateUiState();
    updateConnectionIndicator(false);
    
    emit connectionStateChanged(false, QString());
    
    // Check if we have a pending connection (user wanted to switch hosts)
    if (!m_pendingConnectionHost.isEmpty()) {
        int index = m_hostCombo->findText(m_pendingConnectionHost);
        if (index >= 0) {
            m_hostCombo->setCurrentIndex(index);
        }
        QString pendingHost = m_pendingConnectionHost;
        m_pendingConnectionHost.clear();
        // Give a small delay before connecting to new host
        QTimer::singleShot(100, this, [this, pendingHost]() {
            if (m_hostCombo->currentText() == pendingHost) {
                connectToSelected();
            }
        });
    }
}

void EssHostDiscoveryWidget::onConnectionError(const QString &error)
{
    // Full error goes to console
    EssConsoleManager::instance()->logError(
        QString("Connection error: %1").arg(error), 
        "Discovery"
    );
    
    // Update indicator tooltip with error
    m_connectionIndicator->setToolTip(QString("Error: %1").arg(error));
}

void EssHostDiscoveryWidget::updateConnectionStatus()
{
    QString currentHost = this->currentHost();
    if (!currentHost.isEmpty()) {
        onConnected(currentHost);
    } else {
        updateUiState();
    }
}

void EssHostDiscoveryWidget::updateUiState()
{
    bool connected = !m_connectedHost.isEmpty();
    bool hasSelection = m_hostCombo->currentIndex() >= 0 && 
                       !m_hostCombo->currentText().isEmpty();
    
    m_connectButton->setEnabled(!connected && hasSelection);
    m_disconnectButton->setEnabled(connected);
    
    updateConnectionIndicator(connected);
}

void EssHostDiscoveryWidget::updateConnectionIndicator(bool connected)
{
    if (connected) {
        // Green circle for connected
        m_connectionIndicator->setStyleSheet(
            "QLabel {"
            "  background-color: #57C787;"  // Success green
            "  border: 1px solid #4CAF50;"
            "  border-radius: 6px;"
            "}"
        );
        m_connectionIndicator->setToolTip(QString("Connected to %1").arg(m_connectedHost));
    } else {
        // Red circle for disconnected
        m_connectionIndicator->setStyleSheet(
            "QLabel {"
            "  background-color: #E74C3C;"  // Error red
            "  border: 1px solid #C0392B;"
            "  border-radius: 6px;"
            "}"
        );
        m_connectionIndicator->setToolTip("Disconnected");
    }
}

bool EssHostDiscoveryWidget::isLocalhostRunning()
{
    // Quick check to see if dserv is running on localhost
    DservClient testClient;
    
    // Use the new isHostAvailable method with a 500ms timeout
    // This tests both connectivity and verifies it's actually dserv
    bool available = testClient.isHostAvailable("localhost", 4620, 500);
    
    // Removed debug logging since this runs frequently
    
    return available;
}
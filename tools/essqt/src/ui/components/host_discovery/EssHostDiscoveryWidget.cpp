#include "EssHostDiscoveryWidget.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include "communication/DservClient.h"

#include <QMessageBox>
#include <QRegularExpression>
#include <QThread>
#include <cstring>

// Include the mDNS discovery function
extern "C" {
    int send_mdns_query_service(const char* service_name, char* result_buf, 
                               int result_len, int timeout_ms);
}

EssHostDiscoveryWidget::EssHostDiscoveryWidget(QWidget *parent)
    : QWidget(parent)
    , m_isRefreshing(false)
    , m_commandInterface(nullptr)
{
    setupUi();
    connectSignals();
    
    // Setup refresh timer
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &EssHostDiscoveryWidget::onRefreshTimeout);
    
    // Log initialization
    EssConsoleManager::instance()->logSystem("Host Discovery widget initialized", "Discovery");
    
    // Auto-refresh on startup after a longer delay to ensure network is ready
    QTimer::singleShot(100, this, &EssHostDiscoveryWidget::refreshHosts);
}

EssHostDiscoveryWidget::~EssHostDiscoveryWidget()
{
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
    m_refreshButton->setToolTip("Refresh - Search for available ESS/dserv hosts");
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
    
    // Progress bar
    m_progressBar = new QProgressBar();
    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 0);
    m_progressBar->setMaximumHeight(16);
    m_progressBar->setTextVisible(false);
    
    // Add all to layout
    layout->addWidget(m_hostCombo);
    layout->addWidget(m_refreshButton);
    layout->addWidget(m_connectButton);
    layout->addWidget(m_disconnectButton);
    layout->addWidget(m_progressBar, 1);  // Progress bar takes available space when visible
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
    m_progressBar->setVisible(true);
    m_refreshButton->setEnabled(false);
    
    EssConsoleManager::instance()->logInfo("Starting mDNS discovery for _dserv._tcp", "Discovery");
    
    emit refreshStarted();
    
    // Start the discovery process
    startMdnsDiscovery();
}

void EssHostDiscoveryWidget::startMdnsDiscovery()
{
    // Use a very short timer to make it async but not threaded
    m_refreshTimer->start(50);
}

void EssHostDiscoveryWidget::onRefreshTimeout()
{
    QString result;
    bool success = callMdnsDiscovery(result);
    
    // On initial startup, try once more if first attempt failed
    static bool isInitialRefresh = true;
    if (!success && isInitialRefresh) {
        EssConsoleManager::instance()->logInfo(
            "Initial discovery failed, retrying...", 
            "Discovery"
        );
        
        // Short delay before retry
        QThread::msleep(200);
        
        // Try again
        success = callMdnsDiscovery(result);
        
        if (success) {
            EssConsoleManager::instance()->logSuccess(
                "Retry successful - found hosts", 
                "Discovery"
            );
        }
    }
    isInitialRefresh = false;
    
    m_isRefreshing = false;
    m_progressBar->setVisible(false);
    m_refreshButton->setEnabled(true);
    
    if (success && !result.isEmpty()) {
        parseHostsFromMdns(result);
        
        EssConsoleManager::instance()->logSuccess(
            QString("Discovery complete: found %1 host(s)").arg(m_hostCombo->count()), 
            "Discovery"
        );
    } else {
        // Clear combo first
        m_hostCombo->clear();
        
        // Add localhost as fallback if available
        if (isLocalhostRunning()) {
            m_hostCombo->addItem("localhost");
            
            EssConsoleManager::instance()->logWarning(
                "mDNS discovery failed - using localhost as fallback", 
                "Discovery"
            );
        } else {
            m_hostCombo->setPlaceholderText("No hosts found");
            
            EssConsoleManager::instance()->logWarning(
                "No hosts discovered and localhost dserv not running", 
                "Discovery"
            );
        }
    }
    
    // Update connected host highlighting
    updateConnectionStatus();
    
    emit refreshFinished();
}

bool EssHostDiscoveryWidget::callMdnsDiscovery(QString &result)
{
    const char* service = "_dserv._tcp";
    char buffer[4096];
    int timeout_ms = 250;  
    
    // Clear buffer first
    memset(buffer, 0, sizeof(buffer));
    
    int returnValue = send_mdns_query_service(service, buffer, sizeof(buffer), timeout_ms);
    
    // Success if buffer has content, regardless of return value
    if (strlen(buffer) > 0) {
        result = QString::fromUtf8(buffer);
        EssConsoleManager::instance()->logDebug(
            QString("mDNS response: %1").arg(result.left(100) + "..."), 
            "Discovery"
        );
        return true;
    }
    
    return false;
}

void EssHostDiscoveryWidget::parseHostsFromMdns(const QString &mdnsResponse)
{
    // Save current selection
    QString currentSelection = m_hostCombo->currentText();
    
    // Clear combo
    m_hostCombo->clear();
    
    if (mdnsResponse.isEmpty()) {
        return;
    }
    
    QStringList uniqueHosts;
    
    // Parse the Tcl list format: { IP { dsport 4620 essport 2570 } }
    // Use regex to extract IP addresses
    QRegularExpression regex(R"(\{\s*(\d+\.\d+\.\d+\.\d+)\s*\{)");
    QRegularExpressionMatchIterator iterator = regex.globalMatch(mdnsResponse);
    
    while (iterator.hasNext()) {
        QRegularExpressionMatch match = iterator.next();
        QString ip = match.captured(1);
        
        if (!uniqueHosts.contains(ip)) {
            uniqueHosts << ip;
        }
    }
    
    // If regex fails, try simpler parsing
    if (uniqueHosts.isEmpty()) {
        QStringList parts = mdnsResponse.split(QRegularExpression("[{}\\s]+"), Qt::SkipEmptyParts);
        
        for (const QString &part : parts) {
            // Check if this looks like an IP address
            QStringList octets = part.split('.');
            if (octets.size() == 4) {
                bool validIP = true;
                for (const QString &octet : octets) {
                    bool ok;
                    int val = octet.toInt(&ok);
                    if (!ok || val < 0 || val > 255) {
                        validIP = false;
                        break;
                    }
                }
                
                if (validIP && !uniqueHosts.contains(part)) {
                    uniqueHosts << part;
                }
            }
        }
    }
    
    // Check if localhost should be added (not already in list and is running)
    bool hasLocalhost = uniqueHosts.contains("localhost") || 
                       uniqueHosts.contains("127.0.0.1");
    
    if (!hasLocalhost && isLocalhostRunning()) {
        uniqueHosts.prepend("localhost");
        EssConsoleManager::instance()->logInfo(
            "Added localhost (verified dserv is running)", 
            "Discovery"
        );
    }
    
    // Add hosts to combo box
    if (!uniqueHosts.isEmpty()) {
        m_hostCombo->addItems(uniqueHosts);
        
        // Try to restore previous selection only if we're still connected
        if (!m_connectedHost.isEmpty()) {
            int index = m_hostCombo->findText(m_connectedHost);
            if (index >= 0) {
                m_hostCombo->setCurrentIndex(index);
                // Style the connected item
                m_hostCombo->setItemData(index, QColor(87, 199, 135), Qt::ForegroundRole);
                m_hostCombo->setItemData(index, QFont("", -1, QFont::Bold), Qt::FontRole);
            }
        } else {
            // Not connected - set to placeholder state
            m_hostCombo->setCurrentIndex(-1);
        }
    }
}

void EssHostDiscoveryWidget::connectToSelected()
{
    QString host = m_hostCombo->currentText();
    if (host.isEmpty()) return;
    
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
    
    EssConsoleManager::instance()->logInfo("Disconnecting from host", "Discovery");
    m_commandInterface->disconnectFromHost();
    
    // The actual UI update will happen in onDisconnected() slot when the signal is received
}

void EssHostDiscoveryWidget::onHostSelectionChanged(int index)
{
    bool hasSelection = index >= 0 && !m_hostCombo->currentText().isEmpty();
    QString currentHost = this->currentHost();
    
    m_connectButton->setEnabled(hasSelection && currentHost.isEmpty());
    
    // If user selects a different host while connected, offer to switch
    if (hasSelection && !currentHost.isEmpty()) {
        QString selectedHost = m_hostCombo->currentText();
        if (selectedHost != currentHost) {
            int ret = QMessageBox::question(this, "Already Connected",
                                           QString("Already connected to %1. Disconnect and connect to %2?")
                                           .arg(currentHost).arg(selectedHost),
                                           QMessageBox::Yes | QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                m_pendingConnectionHost = selectedHost;
                disconnectFromHost();
            } else {
                // Restore the connected host in the combo
                int connectedIndex = m_hostCombo->findText(currentHost);
                if (connectedIndex >= 0) {
                    m_hostCombo->setCurrentIndex(connectedIndex);
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
    
    if (available) {
        EssConsoleManager::instance()->logDebug(
            "Localhost dserv is available", 
            "Discovery"
        );
    }
    
    return available;
}
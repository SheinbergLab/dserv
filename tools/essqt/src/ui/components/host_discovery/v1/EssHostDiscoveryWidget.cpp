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
    , m_pendingConnectionItem(nullptr)
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
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(6, 6, 6, 6);
    
    // Create discovery group
    m_discoveryGroup = new QGroupBox("Host Discovery", this);
    auto *groupLayout = new QVBoxLayout(m_discoveryGroup);
    
    // Status and progress
    m_statusLabel = new QLabel("Ready to discover hosts");
    m_statusLabel->setWordWrap(true);
    
    m_progressBar = new QProgressBar();
    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 0); // Indeterminate progress
    
    // Host list
    m_hostList = new QListWidget();
    m_hostList->setMinimumHeight(150);
    m_hostList->setToolTip("Double-click a host to connect");
    m_hostList->setSelectionMode(QAbstractItemView::SingleSelection);
    
    // Buttons
    auto *buttonLayout = new QHBoxLayout();
    
    m_refreshButton = new QPushButton("Refresh");
    m_refreshButton->setToolTip("Search for available ESS/dserv hosts");
    
    m_connectButton = new QPushButton("Connect");
    m_connectButton->setEnabled(false);
    m_connectButton->setToolTip("Connect to selected host");
    
    m_disconnectButton = new QPushButton("Disconnect");
    m_disconnectButton->setEnabled(false);
    m_disconnectButton->setToolTip("Disconnect from current host");
    
    buttonLayout->addWidget(m_refreshButton);
    buttonLayout->addWidget(m_connectButton);
    buttonLayout->addWidget(m_disconnectButton);
    buttonLayout->addStretch();
    
    // Add to group layout
    groupLayout->addWidget(m_statusLabel);
    groupLayout->addWidget(m_progressBar);
    groupLayout->addWidget(m_hostList);
    groupLayout->addLayout(buttonLayout);
    
    // Add group to main layout
    layout->addWidget(m_discoveryGroup);
    layout->addStretch();
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
    connect(m_refreshButton, &QPushButton::clicked, 
            this, &EssHostDiscoveryWidget::refreshHosts);
    connect(m_connectButton, &QPushButton::clicked, 
            this, &EssHostDiscoveryWidget::connectToSelected);
    connect(m_disconnectButton, &QPushButton::clicked, 
            this, &EssHostDiscoveryWidget::disconnectFromHost);
    
    connect(m_hostList, &QListWidget::itemSelectionChanged, 
            this, &EssHostDiscoveryWidget::onHostItemChanged);
    connect(m_hostList, &QListWidget::itemDoubleClicked, 
            this, &EssHostDiscoveryWidget::onHostItemDoubleClicked);
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
    for (int i = 0; i < m_hostList->count(); ++i) {
        QString text = m_hostList->item(i)->text();
        // Remove "(connected)" suffix if present
        if (text.endsWith(" (connected)")) {
            text = text.left(text.length() - 12);
        }
        hosts << text;
    }
    return hosts;
}

void EssHostDiscoveryWidget::refreshHosts()
{
    if (m_isRefreshing) {
        return;
    }
    
    m_isRefreshing = true;
    m_statusLabel->setText("Discovering hosts...");
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
        m_statusLabel->setText(QString("Found %1 host(s)").arg(m_hostList->count()));
        
        EssConsoleManager::instance()->logSuccess(
            QString("Discovery complete: found %1 host(s)").arg(m_hostList->count()), 
            "Discovery"
        );
    } else {
        // Add localhost as fallback
        m_hostList->clear();
        if (isLocalhostRunning()) {
            m_hostList->addItem("localhost");
            m_statusLabel->setText("Discovery failed - added localhost as fallback");
            
            EssConsoleManager::instance()->logWarning(
                "mDNS discovery failed - using localhost as fallback", 
                "Discovery"
            );
        } else {
            m_statusLabel->setText("No hosts found and localhost not available");
            
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
    // Clear current list
    m_hostList->clear();
    
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
    
    // Add hosts to list widget
    for (const QString &host : uniqueHosts) {
        m_hostList->addItem(host);
    }
    
    // Highlight connected host if any
    highlightConnectedHost();
}

void EssHostDiscoveryWidget::connectToSelected()
{
    auto *current = m_hostList->currentItem();
    if (!current) {
        return;
    }
    
    QString host = current->text();
    // Remove "(connected)" suffix if present
    if (host.endsWith(" (connected)")) {
        host = host.left(host.length() - 12);
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
    
    EssConsoleManager::instance()->logInfo("Disconnecting from host", "Discovery");
    m_commandInterface->disconnectFromHost();
    
    // The actual UI update will happen in onDisconnected() slot when the signal is received
}

void EssHostDiscoveryWidget::onHostItemChanged()
{
    bool hasSelection = m_hostList->currentItem() != nullptr;
    QString currentHost = this->currentHost();
    
    m_connectButton->setEnabled(hasSelection && currentHost.isEmpty());
}

void EssHostDiscoveryWidget::onHostItemDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    
    QString currentHost = this->currentHost();
    if (!currentHost.isEmpty()) {
        // Extract the host name without "(connected)" suffix
        QString selectedHost = item->text();
        if (selectedHost.endsWith(" (connected)")) {
            selectedHost = selectedHost.left(selectedHost.length() - 12);
        }
        
        // If clicking on already connected host, do nothing
        if (selectedHost == currentHost) {
            EssConsoleManager::instance()->logInfo("Already connected to this host", "Discovery");
            return;
        }
        
        // Already connected to a different host, ask user
        int ret = QMessageBox::question(this, "Already Connected",
                                       QString("Already connected to %1. Disconnect and connect to %2?")
                                       .arg(currentHost).arg(selectedHost),
                                       QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            // Store the item for reconnection after disconnect
            m_pendingConnectionItem = item;
            disconnectFromHost();
        }
    } else {
        connectToSelected();
    }
}

void EssHostDiscoveryWidget::onConnected(const QString &host)
{
    m_connectedHost = host;
    updateUiState();
    highlightConnectedHost();
    
    m_statusLabel->setText(QString("Connected to %1").arg(host));
    
    emit connectionStateChanged(true, host);
}

void EssHostDiscoveryWidget::onDisconnected()
{
    EssConsoleManager::instance()->logInfo("Disconnect signal received", "Discovery");
    
    QString oldHost = m_connectedHost;
    m_connectedHost.clear();
    
    // Clear any selection to prevent auto-reconnect
    m_hostList->clearSelection();
    m_hostList->setCurrentItem(nullptr);
    
    updateUiState();
    highlightConnectedHost();
    
    m_statusLabel->setText(QString("Disconnected - %1 host(s) available").arg(m_hostList->count()));
    
    emit connectionStateChanged(false, QString());
    
    // Check if we have a pending connection (user wanted to switch hosts)
    if (m_pendingConnectionItem) {
        m_hostList->setCurrentItem(m_pendingConnectionItem);
        m_pendingConnectionItem = nullptr;
        // Give a small delay before connecting to new host
        QTimer::singleShot(100, this, &EssHostDiscoveryWidget::connectToSelected);
    }
}

void EssHostDiscoveryWidget::onConnectionError(const QString &error)
{
    m_statusLabel->setText(QString("Connection error: %1").arg(error));
    
    EssConsoleManager::instance()->logError(
        QString("Connection error: %1").arg(error), 
        "Discovery"
    );
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
    bool hasSelection = m_hostList->currentItem() != nullptr;
    
    m_connectButton->setEnabled(!connected && hasSelection);
    m_disconnectButton->setEnabled(connected);
    
    if (connected) {
        m_statusLabel->setText(QString("Connected to %1").arg(m_connectedHost));
    } else {
        m_statusLabel->setText(QString("Disconnected - %1 host(s) available").arg(m_hostList->count()));
    }
}

void EssHostDiscoveryWidget::highlightConnectedHost()
{
    // Store current selection before updating
    QListWidgetItem *currentSelection = m_hostList->currentItem();
    
    // Update all items to show connection status
    for (int i = 0; i < m_hostList->count(); ++i) {
        QListWidgetItem *item = m_hostList->item(i);
        QString hostText = item->text();
        
        // Remove any existing "(connected)" suffix
        if (hostText.endsWith(" (connected)")) {
            hostText = hostText.left(hostText.length() - 12);
        }
        
        // Check if this is the connected host
        if (!m_connectedHost.isEmpty() && hostText == m_connectedHost) {
            item->setText(hostText + " (connected)");
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            item->setForeground(QColor(87, 199, 135));  // Success green from console
        } else {
            item->setText(hostText);
            QFont font = item->font();
            font.setBold(false);
            item->setFont(font);
            item->setForeground(palette().color(QPalette::Text));
        }
    }
    
    // Restore selection only if it was valid and not connected
    if (currentSelection && !m_connectedHost.isEmpty()) {
        // Don't restore selection if we're connected
        m_hostList->setCurrentItem(nullptr);
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

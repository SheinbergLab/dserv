#include "HostDiscoveryWidget.h"
#include <QDebug>
#include <QMessageBox>
#include <QRegularExpression>

// Include the mDNS discovery function
extern "C" {
    int send_mdns_query_service(const char* service_name, char* result_buf, 
                               int result_len, int timeout_ms);
}

HostDiscoveryWidget::HostDiscoveryWidget(QWidget* parent)
    : QWidget(parent)
    , isRefreshing(false)
{
    setupUI();
    
    // Setup refresh timer
    refreshTimer = new QTimer(this);
    refreshTimer->setSingleShot(true);
    connect(refreshTimer, &QTimer::timeout, this, &HostDiscoveryWidget::onRefreshTimeout);
    
    // Auto-refresh on startup
    QTimer::singleShot(500, this, &HostDiscoveryWidget::refreshHosts);
}

void HostDiscoveryWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(6, 6, 6, 6);
    
    // Create discovery group
    discoveryGroup = new QGroupBox("Host Discovery", this);
    auto* groupLayout = new QVBoxLayout(discoveryGroup);
    
    // Status and progress
    statusLabel = new QLabel("Ready to discover hosts");
    statusLabel->setWordWrap(true);
    
    progressBar = new QProgressBar();
    progressBar->setVisible(false);
    progressBar->setRange(0, 0); // Indeterminate progress
    
    // Host list
    hostList = new QListWidget();
    hostList->setMinimumHeight(150);
    hostList->setToolTip("Double-click a host to connect");
    
    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    
    refreshButton = new QPushButton("Refresh");
    refreshButton->setToolTip("Search for available hosts");
    
    connectButton = new QPushButton("Connect");
    connectButton->setEnabled(false);
    connectButton->setToolTip("Connect to selected host");
    
    disconnectButton = new QPushButton("Disconnect");
    disconnectButton->setEnabled(false);
    disconnectButton->setToolTip("Disconnect from current host");
    
    buttonLayout->addWidget(refreshButton);
    buttonLayout->addWidget(connectButton);
    buttonLayout->addWidget(disconnectButton);
    buttonLayout->addStretch();
    
    // Add to group layout
    groupLayout->addWidget(statusLabel);
    groupLayout->addWidget(progressBar);
    groupLayout->addWidget(hostList);
    groupLayout->addLayout(buttonLayout);
    
    // Add group to main layout
    layout->addWidget(discoveryGroup);
    layout->addStretch();
    
    // Connect signals
    connect(refreshButton, &QPushButton::clicked, this, &HostDiscoveryWidget::refreshHosts);
    connect(connectButton, &QPushButton::clicked, this, &HostDiscoveryWidget::connectToSelected);
    connect(disconnectButton, &QPushButton::clicked, this, &HostDiscoveryWidget::disconnectFromCurrent);
    
    connect(hostList, &QListWidget::itemSelectionChanged, this, &HostDiscoveryWidget::onHostItemChanged);
    connect(hostList, &QListWidget::itemDoubleClicked, this, &HostDiscoveryWidget::onHostItemDoubleClicked);
}

QString HostDiscoveryWidget::currentHost() const {
    return connectedHost;
}

void HostDiscoveryWidget::setCurrentHost(const QString& host) {
    connectedHost = host;
    updateConnectionStatus(host, !host.isEmpty());
}

QStringList HostDiscoveryWidget::discoveredHosts() const {
    QStringList hosts;
    for (int i = 0; i < hostList->count(); ++i) {
        hosts << hostList->item(i)->text();
    }
    return hosts;
}

void HostDiscoveryWidget::refreshHosts() {
    if (isRefreshing) {
        return;
    }
    
    isRefreshing = true;
    statusLabel->setText("Discovering hosts...");
    progressBar->setVisible(true);
    refreshButton->setEnabled(false);
    
    emit refreshStarted();
    
    // Start the discovery process
    startMdnsDiscovery();
}

void HostDiscoveryWidget::startMdnsDiscovery() {
    // Use a very short timer to make it async but not threaded
    refreshTimer->start(50);
}

void HostDiscoveryWidget::onRefreshTimeout() {
    QString result;
    bool success = callMdnsDiscovery(result);
    
    isRefreshing = false;
    progressBar->setVisible(false);
    refreshButton->setEnabled(true);
    
    if (success && !result.isEmpty()) {
        parseHostsFromMdns(result);
        statusLabel->setText(QString("Found %1 host(s)").arg(hostList->count()));
    } else {
        // Add localhost as fallback
        hostList->clear();
        hostList->addItem("localhost");
        statusLabel->setText("Discovery failed - added localhost as fallback");
    }
    
    emit refreshFinished();
}

bool HostDiscoveryWidget::callMdnsDiscovery(QString& result) {
    const char* service = "_dserv._tcp";
    char buffer[4096];
    int timeout_ms = 1000;  // Reasonable timeout
    
    // Clear buffer first
    memset(buffer, 0, sizeof(buffer));
    
    int returnValue = send_mdns_query_service(service, buffer, sizeof(buffer), timeout_ms);
    
    // Success if buffer has content, regardless of return value
    if (strlen(buffer) > 0) {
        result = QString::fromUtf8(buffer);
        return true;
    }
    
    return false;
}

void HostDiscoveryWidget::parseHostsFromMdns(const QString& mdnsResponse) {
    // Clear current list
    hostList->clear();
    
    if (mdnsResponse.isEmpty()) {
        return;
    }
    
    QStringList uniqueHosts;
    
    // Parse the response format: { IP { dsport 4620 essport 2570 } }
    // Use regex to extract IP addresses from the format
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
        
        for (const QString& part : parts) {
            // Check if this looks like an IP address
            QStringList octets = part.split('.');
            if (octets.size() == 4) {
                bool validIP = true;
                for (const QString& octet : octets) {
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
    
    // Add localhost as a default option
    if (!uniqueHosts.contains("localhost")) {
        uniqueHosts.prepend("localhost");
    }
    
    // Add hosts to list widget
    for (const QString& host : uniqueHosts) {
        auto* item = new QListWidgetItem(host);
        
        // Highlight current connected host
        if (host == connectedHost) {
            item->setData(Qt::UserRole, "connected");
            item->setText(host + " (connected)");
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            item->setForeground(QColor(40, 200, 20));
        }
        
        hostList->addItem(item);
    }
}

void HostDiscoveryWidget::connectToSelected() {
    auto* current = hostList->currentItem();
    if (!current) {
        return;
    }
    
    QString host = current->text();
    // Remove "(connected)" suffix if present
    if (host.endsWith(" (connected)")) {
        host = host.left(host.length() - 12);
    }
    
    emit connectRequested(host);
}

void HostDiscoveryWidget::disconnectFromCurrent() {
    emit disconnectRequested();
}

void HostDiscoveryWidget::onHostItemChanged() {
    bool hasSelection = hostList->currentItem() != nullptr;
    connectButton->setEnabled(hasSelection && connectedHost.isEmpty());
}

void HostDiscoveryWidget::onHostItemDoubleClicked() {
    if (!connectedHost.isEmpty()) {
        // Already connected, ask user
        int ret = QMessageBox::question(this, "Already Connected",
                                       QString("Already connected to %1. Disconnect and connect to new host?")
                                       .arg(connectedHost),
                                       QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            disconnectFromCurrent();
            // The actual connection will happen after disconnect is processed
            return;
        } else {
            return;
        }
    }
    
    connectToSelected();
}

void HostDiscoveryWidget::updateConnectionStatus(const QString& host, bool connected) {
    connectedHost = connected ? host : QString();
    
    // Update UI state
    connectButton->setEnabled(!connected && hostList->currentItem() != nullptr);
    disconnectButton->setEnabled(connected);
    
    // Update status
    if (connected) {
        statusLabel->setText(QString("Connected to %1").arg(host));
    } else {
        statusLabel->setText(QString("Disconnected - %1 host(s) available").arg(hostList->count()));
    }
    
    // Refresh the list to update highlighting
    if (hostList->count() > 0) {
        // Don't call mDNS again, just update the display with current hosts
        // (This avoids unnecessary network calls)
    }
}

void HostDiscoveryWidget::setConnectionStatus(const QString& host, bool connected) {
    updateConnectionStatus(host, connected);
}

void HostDiscoveryWidget::onHostConnected(const QString& host) {
    updateConnectionStatus(host, true);
}

void HostDiscoveryWidget::onHostDisconnected() {
    updateConnectionStatus("", false);
}

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QProgressBar>
#include <QGroupBox>

class EssCommandInterface;

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
    void onHostItemChanged();
    void onHostItemDoubleClicked(QListWidgetItem *item);
    void onRefreshTimeout();
    
    // Command interface connection signals
    void onConnected(const QString &host);
    void onDisconnected();
    void onConnectionError(const QString &error);

private:
    void setupUi();
    void connectSignals();
    void parseHostsFromMdns(const QString &mdnsResponse);
    void updateUiState();
    void highlightConnectedHost();
    
    // mDNS discovery
    void startMdnsDiscovery();
    bool callMdnsDiscovery(QString &result);
    
    // UI Components
    QGroupBox *m_discoveryGroup;
    QListWidget *m_hostList;
    QPushButton *m_refreshButton;
    QPushButton *m_connectButton;
    QPushButton *m_disconnectButton;
    QLabel *m_statusLabel;
    QProgressBar *m_progressBar;
    
    // State
    QString m_connectedHost;
    QTimer *m_refreshTimer;
    bool m_isRefreshing;
    QListWidgetItem *m_pendingConnectionItem;
    
    // Command interface reference
    EssCommandInterface *m_commandInterface;
};

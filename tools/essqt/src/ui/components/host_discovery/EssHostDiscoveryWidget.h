#pragma once

#include <QWidget>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QProgressBar>
#include <QToolButton>

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
    void onHostSelectionChanged(int index);
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
    void updateConnectionIndicator(bool connected);
    
    // mDNS discovery
    void startMdnsDiscovery();
    bool callMdnsDiscovery(QString &result);
    
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
};
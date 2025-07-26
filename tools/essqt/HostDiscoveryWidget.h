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

class HostDiscoveryWidget : public QWidget {
    Q_OBJECT

public:
    explicit HostDiscoveryWidget(QWidget* parent = nullptr);

    QString currentHost() const;
    void setCurrentHost(const QString& host);
    QStringList discoveredHosts() const;

public slots:
    void refreshHosts();
    void connectToSelected();
    void disconnectFromCurrent();

    void setConnectionStatus(const QString& host, bool connected);
    void onHostConnected(const QString& host);
    void onHostDisconnected();
  
signals:
    void hostSelected(const QString& host);
    void connectRequested(const QString& host);
    void disconnectRequested();
    void refreshStarted();
    void refreshFinished();

private slots:
    void onHostItemChanged();
    void onHostItemDoubleClicked();
    void onRefreshTimeout();

private:
    void setupUI();
    void updateConnectionStatus(const QString& host, bool connected);
    void parseHostsFromMdns(const QString& mdnsResponse);
    
    // UI Components
    QGroupBox* discoveryGroup;
    QListWidget* hostList;
    QPushButton* refreshButton;
    QPushButton* connectButton;
    QPushButton* disconnectButton;
    QLabel* statusLabel;
    QProgressBar* progressBar;
    
    // State
    QString connectedHost;
    QTimer* refreshTimer;
    bool isRefreshing;
    
    // Discovery
    void startMdnsDiscovery();
    bool callMdnsDiscovery(QString& result);
};

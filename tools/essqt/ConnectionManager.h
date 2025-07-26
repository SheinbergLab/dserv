#pragma once
#include <QObject>
#include <QString>
#include "DservClient.h"
#include "EssClient.h"
#include "DservListener.h"

class ConnectionManager : public QObject {
    Q_OBJECT
    
public:
    explicit ConnectionManager(QObject* parent = nullptr);
    ~ConnectionManager();
    
    // Connection management
    bool connectToHost(const QString& host);
    void disconnectFromHost();
    bool isConnected() const;
    QString currentHost() const { return connectedHost; }
    
    // Command interfaces
    bool sendDservCommand(const QString& command, QString& response);
    bool sendEssCommand(const QString& command, QString& response);
    
    // Convenience methods for dserv operations
    bool getDservValue(const QString& key, QString& value);
    bool getDservKeys(QString& keys);
    bool touchDservVariable(const QString& var);
    
    // Listener management
    bool startListener();
    void stopListener();
    quint16 listenerPort() const;

signals:
    void connected(const QString& host);
    void disconnected();
    void receivedEvent(const QString& event);
    void errorOccurred(const QString& error);

private slots:
    void onListenerEvent(const QString& event);

private:
    void createClients();        // Create fresh clients for connection
    void teardownAllClients();   // Destroy all clients
    
    // All clients are created fresh per connection
    DservClient* dservClient;    // nullptr when not connected
    EssClient* essClient;        // nullptr when not connected (synchronous only)
    DservListener* listener;     // nullptr when not connected
    
    QString connectedHost;
    bool connectionEstablished;
    bool isShuttingDown;  
};

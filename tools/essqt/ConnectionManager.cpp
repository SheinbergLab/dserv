#include "ConnectionManager.h"
#include <QDebug>

ConnectionManager::ConnectionManager(QObject* parent)
    : QObject(parent)
    , dservClient(nullptr)      
    , essClient(nullptr)
    , listener(nullptr)
    , connectionEstablished(false)
    , isShuttingDown(false)
{
}

ConnectionManager::~ConnectionManager() {
    if (connectionEstablished || !connectedHost.isEmpty()) {
        disconnectFromHost();
    }
}

bool ConnectionManager::connectToHost(const QString& host) {
    if (connectionEstablished) {
        disconnectFromHost();
    }
    
    connectedHost = host;
    
    // Create fresh clients for this connection
    createClients();
    
    // Create and start listener
    listener = new DservListener(this);
    connect(listener, &DservListener::receivedEvent, this, &ConnectionManager::onListenerEvent);
    
    if (!listener->startListening()) {
        qDebug() << "FAILED: Could not start new listener";
        teardownAllClients();
        return false;
    }
    
    // Connect ess client (synchronous - no signals needed)
    if (!essClient->connectToHost(host, 2560)) {
        qDebug() << "FAILED: ESS client connection failed";
        teardownAllClients();
        return false;
    }
    
    // Register with dserv and subscribe to events
    quint16 localPort = listener->port();
    
    if (!dservClient->registerListener(host, localPort)) {
        qDebug() << "FAILED: Could not register listener with dserv";
        teardownAllClients();
        return false;
    }

    // Subscribe to ess events
    if (!dservClient->subscribeMatch(host, localPort, "ess/*")) {
        qDebug() << "FAILED: Could not subscribe to ess/* events";
        teardownAllClients();
        return false;
    }
    
    // Subscribe to additional events
    dservClient->subscribeMatch(host, localPort, "system/*");
    dservClient->subscribeMatch(host, localPort, "stimdg");
    dservClient->subscribeMatch(host, localPort, "trialdg");
    dservClient->subscribeMatch(host, localPort, "openiris/settings");
    dservClient->subscribeMatch(host, localPort, "print");
    
    connectionEstablished = true;
    
    // Touch variables to get initial state
    QStringList varsToTouch = {
        "ess/systems", "ess/protocols", "ess/variants", "ess/system", "ess/protocol",
        "ess/variant", "ess/subject", "ess/state", "ess/em_pos", "ess/obs_id", "ess/obs_total",
        "ess/block_pct_complete", "ess/block_pct_correct", "ess/variant_info", "ess/screen_w",
        "ess/screen_h", "ess/screen_halfx", "ess/screen_halfy", "ess/state_table", "ess/rmt_cmds",
        "ess/system_script", "ess/protocol_script", "ess/variants_script", "ess/loaders_script",
        "ess/stim_script", "ess/param_settings", "ess/params", "stimdg", "trialdg",
        "ess/git/branches", "ess/git/branch", "system/hostname", "system/os", "openiris/settings"
    };
    
    QString touchCmd = QString("foreach v {%1} { dservTouch $v }").arg(varsToTouch.join(" "));
    QString response;
    
    // Use synchronous command but don't fail connection if it doesn't work
    bool touchResult = sendEssCommand(touchCmd, response);
    if (!touchResult) {
        qDebug() << "Warning: Touch command failed, but continuing with connection";
    }
    
    emit connected(host);
    return true;
}

void ConnectionManager::disconnectFromHost() {
    
    if (!connectionEstablished && connectedHost.isEmpty()) {
        qDebug() << "Not connected, nothing to disconnect";
        return;
    }
    
    isShuttingDown = true;
    
    // Quick unregister - don't wait long for response during shutdown
    if (!connectedHost.isEmpty() && listener && dservClient) {
        // Use shorter timeout for shutdown
        dservClient->unregisterListener(connectedHost, listener->port());
    }
    
    // Disconnect ESS client quickly
    if (essClient && essClient->isConnected()) {
        essClient->disconnectFromHost();
    }
    
    // Completely tear down all clients
    teardownAllClients();
    
    connectionEstablished = false;
    connectedHost.clear();
    isShuttingDown = false;
    
    emit disconnected();
}

void ConnectionManager::createClients() {
    
    // Clean up any existing clients first
    if (dservClient) {
        dservClient->deleteLater();
        dservClient = nullptr;
    }
    if (essClient) {
        essClient->deleteLater();
        essClient = nullptr;
    }
    
    // Create fresh dserv client (pure synchronous - no signals needed)
    dservClient = new DservClient(this);
    
    // Create fresh ess client (pure synchronous - no signals needed)
    essClient = new EssClient(this);
}

void ConnectionManager::teardownAllClients() {
    if (listener) {
        listener->shutdown();  // Fast shutdown instead of just deleteLater
        listener->deleteLater();
        listener = nullptr;
    }
    
    if (essClient) {
        essClient->deleteLater();
        essClient = nullptr;
    }
    
    if (dservClient) {
        dservClient->deleteLater();
        dservClient = nullptr;
    }
}

bool ConnectionManager::isConnected() const {
    return connectionEstablished && 
           essClient && essClient->isConnected() && 
           !connectedHost.isEmpty();
}

// Command methods with null checks and better error handling
bool ConnectionManager::sendDservCommand(const QString& command, QString& response) {
    if (!connectionEstablished || !dservClient) {
        qDebug() << "Cannot send dserv command: not connected or no dserv client";
        return false;
    }
    
    bool result = dservClient->sendCommand(connectedHost, 4620, command, response);
    if (!result) {
        qDebug() << "Dserv command failed. Last error:" << dservClient->lastError();
    }
    return result;
}

bool ConnectionManager::sendEssCommand(const QString& command, QString& response) {
    if (!connectionEstablished || !essClient) {
        qDebug() << "Cannot send ess command: not connected or no ess client";
        return false;
    }
    
    bool result = essClient->sendCommand(command, response, 5000); // 5 second timeout
    return result;
}

bool ConnectionManager::getDservValue(const QString& key, QString& value) {
    if (!connectionEstablished || !dservClient) {
        return false;
    }
    
    bool result = dservClient->getValue(connectedHost, key, value);
    if (!result && !dservClient->lastError().isEmpty()) {
        qDebug() << "Get dserv value failed. Last error:" << dservClient->lastError();
    }
    return result;
}

bool ConnectionManager::getDservKeys(QString& keys) {
    if (!connectionEstablished || !dservClient) {
        return false;
    }
    
    bool result = dservClient->getKeys(connectedHost, keys);
    if (!result && !dservClient->lastError().isEmpty()) {
        qDebug() << "Get dserv keys failed. Last error:" << dservClient->lastError();
    }
    return result;
}

bool ConnectionManager::touchDservVariable(const QString& var) {
    if (!connectionEstablished || !dservClient) {
        return false;
    }
    
    bool result = dservClient->touch(connectedHost, var);
    if (!result && !dservClient->lastError().isEmpty()) {
        qDebug() << "Touch dserv variable failed. Last error:" << dservClient->lastError();
    }
    return result;
}

// Simplified listener management
bool ConnectionManager::startListener() {
    return listener && listener->startListening();
}

void ConnectionManager::stopListener() {
    teardownAllClients();
}

quint16 ConnectionManager::listenerPort() const {
    return listener ? listener->port() : 0;
}

// Event handlers - only listener events now
void ConnectionManager::onListenerEvent(const QString& event) {
    emit receivedEvent(event);
}

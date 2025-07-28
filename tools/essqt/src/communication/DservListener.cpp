// DservListener.cpp
#include "DservListener.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

DservListener::DservListener(QObject* parent) : QTcpServer(parent) {}

DservListener::~DservListener() {
    shutdown();
}

bool DservListener::startListening() {
    return listen(QHostAddress::Any, 0);  // bind to random port
}

quint16 DservListener::port() const {
    return serverPort();
}

void DservListener::shutdown() {
    if (isListening()) {
        
        // Stop accepting new connections
        close();
        
        // Force close all client connections immediately
        for (QTcpSocket* client : clients) {
            if (client) {
                client->abort();  // Force immediate close
                client->deleteLater();
            }
        }
        
        clients.clear();
        buffers.clear();
        
    }
}

void DservListener::incomingConnection(qintptr socketDescriptor) {
    QTcpSocket* client = new QTcpSocket(this);
    client->setSocketDescriptor(socketDescriptor);
    connect(client, &QTcpSocket::readyRead, this, &DservListener::readFromClient);
    connect(client, &QTcpSocket::disconnected, this, &DservListener::clientDisconnected);
    clients.append(client);
    buffers[client] = QByteArray();
}

void DservListener::readFromClient() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    buffers[client] += client->readAll();
    while (true) {
        int newlineIndex = buffers[client].indexOf('\n');
        if (newlineIndex == -1) break;

        QByteArray line = buffers[client].left(newlineIndex);
        buffers[client].remove(0, newlineIndex + 1);
        emit receivedEvent(QString::fromUtf8(line));
    }
}

void DservListener::clientDisconnected() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;
    
    clients.removeAll(client);
    buffers.remove(client);
    client->deleteLater();
}

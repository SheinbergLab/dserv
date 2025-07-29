#include "EssClient.h"
#include <QDebug>
#include <QDataStream>
#include <arpa/inet.h>

EssClient::EssClient(QObject* parent) 
    : QObject(parent)
    , socket(new QTcpSocket(this))
    , currentPort(2560)
{
    // NO signal/slot connections - pure synchronous operation
}

EssClient::~EssClient()
{
    disconnectFromHost();
}

bool EssClient::connectToHost(const QString& host, quint16 port) {
    if (socket->state() == QAbstractSocket::ConnectedState) {
        disconnectFromHost();
    }
    
    currentHost = host;
    currentPort = port;
    
    socket->connectToHost(host, port);
    bool connected = socket->waitForConnected(3000);
    
    if (!connected) {
        qDebug() << "[EssClient] Connection error:" << socket->errorString();
    }
    
    return connected;
}

void EssClient::disconnectFromHost() {
    if (socket->state() == QAbstractSocket::ConnectedState || 
        socket->state() == QAbstractSocket::ConnectingState) {
        
        socket->disconnectFromHost();
        
        // Shorter timeout for shutdown - don't wait long
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->waitForDisconnected(500);  // Reduced from 1000ms
        }
        
        // Force close if still not disconnected
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            qDebug() << "[EssClient] Force closing socket";
            socket->abort();
        }
    }
}

bool EssClient::isConnected() const {
    return socket->state() == QAbstractSocket::ConnectedState;
}

bool EssClient::sendCommand(const QString& command, QString& response, int timeoutMs) {
    if (!isConnected()) {
        qDebug() << "[sendCommand] Not connected";
        return false;
    }

    // Send the message
    if (!sendMessage(command)) {
        qDebug() << "[sendCommand] Failed to send message";
        return false;
    }

    // Receive the response
    if (!receiveMessage(response, timeoutMs)) {
        qDebug() << "[sendCommand] Failed to receive response";
        return false;
    }
    return true;
}

bool EssClient::sendAsyncCommand(const QString& command, QString& response, int timeoutMs) {
	QString noreply_command = QString("evalNoReply {%1}").arg(command);
	return sendCommand(noreply_command, response, timeoutMs);
}

bool EssClient::sendMessage(const QString& message) {
    QByteArray data = message.toUtf8();
    quint32 size = data.size();
    
    // Build packet: 4-byte length (network order) + message data
    QByteArray packet;
    quint32 networkSize = htonl(size);
    packet.append(reinterpret_cast<const char*>(&networkSize), 4);
    packet.append(data);
    
    qint64 written = socket->write(packet);
    if (written != packet.size()) {
        qDebug() << "[sendMessage] Write size mismatch: expected" << packet.size() << "wrote" << written;
        return false;
    }
    
    if (!socket->waitForBytesWritten(3000)) {
        qDebug() << "[sendMessage] Write timeout:" << socket->errorString();
        return false;
    }
    
    return true;
}

bool EssClient::receiveMessage(QString& response, int timeoutMs) {
    // Wait for data to arrive
    if (!socket->waitForReadyRead(timeoutMs)) {
        qDebug() << "[receiveMessage] Timeout waiting for data:" << socket->errorString();
        qDebug() << "[receiveMessage] Socket state:" << socket->state();
        return false;
    }
    
    // Read all available data (like our working test)
    QByteArray rawData = socket->readAll();
    
    // Parse the message protocol
    if (rawData.size() < 4) {
        qDebug() << "[receiveMessage] Response too short for header";
        return false;
    }
    
    // Extract message length
    quint32 messageLength;
    memcpy(&messageLength, rawData.constData(), 4);
    messageLength = ntohl(messageLength);
    
    if (messageLength == 0) {
        response.clear();
        return true;
    }
    
    if (rawData.size() < 4 + messageLength) {
        qDebug() << "[receiveMessage] Incomplete message: need" << (4 + messageLength) 
                 << "bytes, have" << rawData.size();
        return false;
    }
    
    // Extract the message content
    response = QString::fromUtf8(rawData.mid(4, messageLength));
    
    return true;
}

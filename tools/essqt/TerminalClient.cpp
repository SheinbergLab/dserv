// TerminalClient.cpp
#include "TerminalClient.h"
#include <QDataStream>

TerminalClient::TerminalClient(QObject *parent) : QObject(parent) {
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected, this, &TerminalClient::onConnected);
    connect(socket, &QTcpSocket::readyRead, this, &TerminalClient::onReadyRead);
    connect(socket, &QTcpSocket::errorOccurred, this, &TerminalClient::onSocketError);
}

void TerminalClient::connectToServer(const QString &host, int port) {
    socket->connectToHost(host, port);
}

void TerminalClient::onConnected() {
    // Optional: log or emit a signal
    // For now, you can leave it empty or add a debug message
}

void TerminalClient::sendMessage(const QString &message) {
    QByteArray msg = message.toUtf8();
    quint32 size = msg.size();
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << size;
    packet.append(msg);
    socket->write(packet);
}

void TerminalClient::onReadyRead() {
    buffer.append(socket->readAll());

    while (true) {
        if (expectedSize == 0) {
            if (buffer.size() < 4) return;
            QDataStream stream(buffer);
            stream.setByteOrder(QDataStream::BigEndian);
            stream >> expectedSize;
            buffer.remove(0, 4);
        }

        if (buffer.size() < expectedSize) return;

        QByteArray msg = buffer.left(expectedSize);
        buffer.remove(0, expectedSize);
        expectedSize = 0;

        emit messageReceived(QString::fromUtf8(msg));
    }
}

void TerminalClient::onSocketError(QAbstractSocket::SocketError) {
    emit errorOccurred(socket->errorString());
}

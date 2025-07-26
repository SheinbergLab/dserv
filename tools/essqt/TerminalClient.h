// TerminalClient.h
#pragma once
#include <QObject>
#include <QTcpSocket>

class TerminalClient : public QObject {
    Q_OBJECT

public:
    TerminalClient(QObject *parent = nullptr);
    void connectToServer(const QString &host, int port);

signals:
    void messageReceived(const QString &response);
    void errorOccurred(const QString &error);

private slots:
    void onConnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError);

public slots:
  void sendMessage(const QString &msg);
  
private:
    QTcpSocket *socket;
    quint32 expectedSize = 0;
    QByteArray buffer;
};

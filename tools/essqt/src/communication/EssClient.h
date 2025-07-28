#ifndef ESSCLIENT_H
#define ESSCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QString>

class EssClient : public QObject
{
    Q_OBJECT

public:
    explicit EssClient(QObject* parent = nullptr);
    ~EssClient();

    // Connection management
    bool connectToHost(const QString& host, quint16 port = 2560);
    void disconnectFromHost();
    bool isConnected() const;

    // Synchronous command execution
    bool sendCommand(const QString& command, QString& response, int timeoutMs = 5000);

private:
    // Helper methods for synchronous communication
    bool sendMessage(const QString& message);
    bool receiveMessage(QString& response, int timeoutMs);

    // Socket and connection info
    QTcpSocket* socket;
    QString currentHost;
    quint16 currentPort;
};

#endif // ESSCLIENT_H

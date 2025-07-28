// DservListener.h
#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>

class DservListener : public QTcpServer {
    Q_OBJECT
public:
    explicit DservListener(QObject* parent = nullptr);
    ~DservListener();
    
    bool startListening();  // binds to random port
    quint16 port() const;
    void shutdown();  // Fast shutdown method

signals:
    void receivedEvent(const QString& event);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void readFromClient();
    void clientDisconnected();

private:
    QList<QTcpSocket*> clients;
    QHash<QTcpSocket*, QByteArray> buffers;
};

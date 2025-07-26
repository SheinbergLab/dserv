#include "DservClient.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QDebug>

DservClient::DservClient(QObject* parent) : QObject(parent) {}

QString DservClient::getLocalIP() {
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol && 
            !addr.isLoopback() && 
            !addr.toString().startsWith("169.254")) { // Skip link-local
            return addr.toString();
        }
    }
    return "127.0.0.1";
}

void DservClient::setError(const QString& error) {
    m_lastError = error;
    qDebug() << "[DservClient Error]" << error;
}

DservClient::DservResponse DservClient::executeCommand(const QString& host, quint16 port, const QString& cmd, const QString& operation) {
    DservResponse result;
    
    QTcpSocket socket;
    
    socket.connectToHost(host, port);
    
    if (!socket.waitForConnected(3000)) {
        QString error = QString("Failed to connect to %1:%2 - %3").arg(host).arg(port).arg(socket.errorString());
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.networkSuccess = false;
        result.status = StatusNetworkError;
        return result;
    }

    // Ensure command ends with newline for dserv protocol
    QString command = cmd;
    if (!command.endsWith('\n')) {
        command += '\n';
    }

    QByteArray msg = command.toUtf8();
    qint64 bytesWritten = socket.write(msg);
    
    if (bytesWritten != msg.size()) {
        QString error = QString("Failed to write complete command (%1/%2 bytes)").arg(bytesWritten).arg(msg.size());
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.networkSuccess = false;
        return result;
    }
    
    if (!socket.waitForBytesWritten(3000)) {
        QString error = QString("Timeout writing command - %1").arg(socket.errorString());
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.networkSuccess = false;
        return result;
    }
    
    if (!socket.waitForReadyRead(3000)) {
        QString error = QString("Timeout waiting for response - %1").arg(socket.errorString());
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.networkSuccess = false;
        return result;
    }

    QByteArray responseData = socket.readAll();
    result.rawResponse = QString::fromUtf8(responseData).trimmed();
    result.networkSuccess = true;
    
    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(200);  // Shorter timeout for faster shutdown
    }
    
    // Parse the response
    if (result.rawResponse.isEmpty()) {
        QString error = "Empty response from server";
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.status = StatusParseError;
        return result;
    }
    
    // Parse response - format is "STATUS DATA" where STATUS is the first word
    int spaceIndex = result.rawResponse.indexOf(' ');
    if (spaceIndex <= 0) {
        // No space found - might be just a status code with no data
        bool ok;
        int status = result.rawResponse.toInt(&ok);
        if (ok) {
            result.status = static_cast<DservStatus>(status);
            result.data = "";  // No data portion
        } else {
            QString error = QString("Invalid response format: %1").arg(result.rawResponse);
            if (!operation.isEmpty()) {
                error = QString("%1: %2").arg(operation, error);
            }
            setError(error);
            result.status = StatusParseError;
        }
        return result;
    }
    
    QString statusStr = result.rawResponse.left(spaceIndex);
    bool ok;
    int status = statusStr.toInt(&ok);
    
    if (!ok) {
        QString error = QString("Invalid status code: %1").arg(statusStr);
        if (!operation.isEmpty()) {
            error = QString("%1: %2").arg(operation, error);
        }
        setError(error);
        result.status = StatusParseError;
        return result;
    }
    
    // Extract data after the status and space
    result.status = static_cast<DservStatus>(status);
    result.data = result.rawResponse.mid(spaceIndex + 1);
    
    return result;
}

// Legacy interface for backward compatibility
bool DservClient::sendCommand(const QString& host, quint16 port, const QString& cmd, QString& response) {
    DservResponse result = executeCommand(host, port, cmd);
    response = result.rawResponse;
    return result.networkSuccess;
}

bool DservClient::getValue(const QString& host, const QString& key, QString& value, quint16 dservPort) {
    QString cmd = QString("%get %1").arg(key);
    DservResponse result = executeCommand(host, dservPort, cmd, QString("Get value for key '%1'").arg(key));
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess()) {
        value = result.data;
        return true;
    } else if (result.isNotFound()) {
        return false;
    } else if (result.isError()) {
        setError(QString("Server error getting value for key: %1").arg(key));
        return false;
    } else {
        // Parse error - already handled by executeCommand
        return false;
    }
}

bool DservClient::getKeys(const QString& host, QString& keys, quint16 dservPort) {
    QString cmd = "%getkeys";
    DservResponse result = executeCommand(host, dservPort, cmd, "Get keys");
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess()) {
        keys = result.data;
        return true;
    } else if (result.isNotFound()) {
        keys = "";
        return true;
    } else if (result.isError()) {
        setError("Server error getting keys");
        return false;
    } else {
        // Parse error - already handled by executeCommand
        return false;
    }
}

bool DservClient::registerListener(const QString& host, quint16 localPort, quint16 dservPort) {
    QString ip = getLocalIP();
    QString cmd = QString("%reg %1 %2 2").arg(ip).arg(localPort);
    DservResponse result = executeCommand(host, dservPort, cmd, "Register listener");
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess()) {
        return true;
    } else {
        setError(QString("Failed to register listener: status %1").arg(static_cast<int>(result.status)));
        return false;
    }
}

bool DservClient::unregisterListener(const QString& host, quint16 localPort, quint16 dservPort) {
    QString ip = getLocalIP();
    QString cmd = QString("%unreg %1 %2").arg(ip).arg(localPort);
    DservResponse result = executeCommand(host, dservPort, cmd, "Unregister listener");
    
    if (result.isNetworkError()) {
        // During shutdown, network errors are expected
        qDebug() << "[DservClient] Network error during unregister (expected during shutdown)";
        return false;
    }
    
    if (result.isSuccess()) {
        return true;
    } else {
        // Log but don't set error - unregister during shutdown might fail if server is gone
        qDebug() << "[DservClient] Unregister listener warning: status" << static_cast<int>(result.status);
        return false;
    }
}

bool DservClient::subscribeMatch(const QString& host, quint16 localPort, 
                                const QString& match, int every, quint16 dservPort) {
    QString ip = getLocalIP();
    QString cmd = QString("%match %1 %2 %3 %4").arg(ip).arg(localPort).arg(match).arg(every);
    DservResponse result = executeCommand(host, dservPort, cmd, QString("Subscribe to match '%1'").arg(match));
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess()) {
        return true;
    } else {
        setError(QString("Failed to subscribe to match '%1': status %2").arg(match).arg(static_cast<int>(result.status)));
        return false;
    }
}

bool DservClient::removeMatch(const QString& host, quint16 localPort, 
                             const QString& match, quint16 dservPort) {
    QString ip = getLocalIP();
    QString cmd = QString("%unmatch %1 %2 %3").arg(ip).arg(localPort).arg(match);
    DservResponse result = executeCommand(host, dservPort, cmd, QString("Remove match '%1'").arg(match));
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess()) {
        return true;
    } else {
        setError(QString("Failed to remove match '%1': status %2").arg(match).arg(static_cast<int>(result.status)));
        return false;
    }
}

bool DservClient::touch(const QString& host, const QString& var, quint16 dservPort) {
    QString cmd = QString("%touch %1").arg(var);
    DservResponse result = executeCommand(host, dservPort, cmd, QString("Touch variable '%1'").arg(var));
    
    if (result.isNetworkError()) {
        return false;
    }
    
    if (result.isSuccess() || result.isNotFound()) {
        return true;
    } else {
        setError(QString("Failed to touch variable '%1': status %2").arg(var).arg(static_cast<int>(result.status)));
        return false;
    }
}

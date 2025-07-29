#pragma once
#include <QObject>
#include <QString>

class DservClient : public QObject {
    Q_OBJECT

public:
    explicit DservClient(QObject* parent = nullptr);
    
    // Core dserv operations (port 4620)
    bool registerListener(const QString& host, quint16 localPort, quint16 dservPort = 4620);
    bool unregisterListener(const QString& host, quint16 localPort, quint16 dservPort = 4620);
    bool subscribeMatch(const QString& host, quint16 localPort, const QString& match, 
                       int every = 1, quint16 dservPort = 4620);
    bool removeMatch(const QString& host, quint16 localPort, const QString& match, 
                    quint16 dservPort = 4620);
    bool touch(const QString& host, const QString& var, quint16 dservPort = 4620);
    
    // Generic dserv command (for backward compatibility)
    bool sendCommand(const QString& host, quint16 port, const QString& cmd, QString& response);
    
    // Convenience methods for common dserv operations
    bool getValue(const QString& host, const QString& key, QString& value, quint16 dservPort = 4620);
    bool getKeys(const QString& host, QString& keys, quint16 dservPort = 4620);
    
    // Connection testing
    bool testConnection(const QString& host, quint16 port = 4620, int timeoutMs = 1000);
    bool isHostAvailable(const QString& host, quint16 port = 4620, int timeoutMs = 1000);
    
    // Get last error message for debugging
    QString lastError() const { return m_lastError; }
    void clearError() { m_lastError.clear(); }

private:
    enum DservStatus {
        StatusSuccess = 1,
        StatusNotFound = 0,
        StatusError = -1,
        StatusNetworkError = -998,
        StatusParseError = -999
    };
    
    struct DservResponse {
        DservStatus status;
        QString data;
        QString rawResponse;
        bool networkSuccess;
        
        DservResponse() : status(StatusNetworkError), networkSuccess(false) {}
        
        bool isSuccess() const { return status == StatusSuccess; }
        bool isNotFound() const { return status == StatusNotFound; }
        bool isError() const { return status == StatusError; }
        bool isNetworkError() const { return !networkSuccess; }
        bool isParseError() const { return status == StatusParseError; }
    };
    
    QString getLocalIP();
    DservResponse executeCommand(const QString& host, quint16 port, const QString& cmd, const QString& operation = "");
    void setError(const QString& error);
    
    QString m_lastError;
};

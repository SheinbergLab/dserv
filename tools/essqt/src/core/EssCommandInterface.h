#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class DservClient;
class EssClient;
class DservListener;
class DservEventParser;
class QThread;

// Forward declare for Tcl
struct Tcl_Interp;

class EssCommandInterface : public QObject
{
    Q_OBJECT

public:
    enum CommandChannel {
        ChannelLocal,    // Local Tcl interpreter
        ChannelDserv,    // DservClient (port 4620)
        ChannelEss,      // EssClient (port 2560)
        ChannelAuto      // Auto-detect based on command
    };

    enum CommandStatus {
        StatusSuccess,
        StatusError,
        StatusTimeout,
        StatusNotConnected
    };

    struct CommandResult {
        CommandStatus status;
        QString response;
        QString error;
        CommandChannel channel;
    };

    explicit EssCommandInterface(QObject *parent = nullptr);
    ~EssCommandInterface();

    // Connection management
    bool connectToHost(const QString &host);
    void disconnectFromHost();
    bool isConnected() const;
    QString currentHost() const { return m_currentHost; }
    
    // Listener management
    bool startListener();
    void stopListener();
    bool isListening() const;
    quint16 listenerPort() const;
    
    // Subscription management
    bool subscribe(const QString &pattern, int every = 1);
    bool unsubscribe(const QString &pattern);
    void clearSubscriptions();

    // Command execution
    CommandResult executeCommand(const QString &command, CommandChannel channel = ChannelAuto);
    
    // Async command execution
    void executeCommandAsync(const QString &command, CommandChannel channel = ChannelAuto);

    // Channel detection
    CommandChannel detectChannel(const QString &command) const;
    
    // Channel management
    void setDefaultChannel(CommandChannel channel) { m_defaultChannel = channel; }
    CommandChannel defaultChannel() const { return m_defaultChannel; }
    QString channelName(CommandChannel channel) const;

    // Get available commands for auto-completion
    QStringList getAvailableCommands() const;
    QStringList getTclCommands() const;

signals:
    void connected(const QString &host);
    void disconnected();
    void commandCompleted(const CommandResult &result);
    void connectionError(const QString &error);
    
    // Datapoint updates from listener
    void datapointUpdated(const QString &name, const QVariant &value, qint64 timestamp);

private slots:
    void onEventReceived(const QString &event);

private:
    // Initialize Tcl interpreter
    void initializeTcl();
    void shutdownTcl();

    // Execute on specific channels
    CommandResult executeLocalTcl(const QString &command);
    CommandResult executeDserv(const QString &command);
    CommandResult executeEss(const QString &command);

    // Member variables
    std::unique_ptr<DservClient> m_dservClient;
    std::unique_ptr<EssClient> m_essClient;
    std::unique_ptr<DservListener> m_dservListener;
    std::unique_ptr<DservEventParser> m_eventParser;
    Tcl_Interp *m_tclInterp;
    
    QString m_currentHost;
    bool m_isConnected;
    CommandChannel m_defaultChannel;
    QStringList m_activeSubscriptions;

    // Common ESS commands for auto-detection
    static const QStringList s_essCommands;
    static const QStringList s_dservCommands;
};

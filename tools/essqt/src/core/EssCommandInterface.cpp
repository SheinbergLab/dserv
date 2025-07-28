#include "EssCommandInterface.h"
#include "EssApplication.h"
#include "core/EssDataProcessor.h"
#include "communication/DservClient.h"
#include "communication/EssClient.h"
#include "communication/DservListener.h"
#include "communication/DservEventParser.h"
#include "console/EssOutputConsole.h"

#include <QDebug>
#include <tcl.h>

// Static member initialization
const QStringList EssCommandInterface::s_essCommands = {
    "::ess::load_system", "::ess::reload_system", "::ess::start", "::ess::stop", 
    "::ess::reset", "::ess::save_script", "::ess::set_param", "::ess::get_param",
    "::ess::list_systems", "::ess::list_protocols", "::ess::list_variants",
    "::ess::get_status", "::ess::get_system", "::ess::get_protocol", "::ess::get_variant"
};

const QStringList EssCommandInterface::s_dservCommands = {
    "%get", "%set", "%getkeys", "%match", "%unmatch", "%touch", "%reg", "%unreg",
    "%subscribe", "%unsubscribe", "%list", "%status"
};

EssCommandInterface::EssCommandInterface(QObject *parent)
    : QObject(parent)
    , m_dservClient(std::make_unique<DservClient>())
    , m_essClient(std::make_unique<EssClient>())
    , m_dservListener(std::make_unique<DservListener>())
    , m_eventParser(std::make_unique<DservEventParser>())
    , m_tclInterp(nullptr)
    , m_isConnected(false)
    , m_defaultChannel(ChannelLocal)  // Start in local Tcl mode
{
    initializeTcl();
    
    // Connect listener signals
    connect(m_dservListener.get(), &DservListener::receivedEvent,
            this, &EssCommandInterface::onEventReceived);
}

EssCommandInterface::~EssCommandInterface()
{
    // Stop listener first (it might be sending updates)
    if (m_dservListener) {
        m_dservListener->shutdown();
    }
    
    // Clear active state without logging
    if (m_isConnected) {
        m_isConnected = false;
        m_activeSubscriptions.clear();
        
        // Clients will clean up in their destructors
    }
    
    // Shutdown Tcl
    shutdownTcl();
    
    // Let unique_ptrs handle cleanup
}

void EssCommandInterface::initializeTcl()
{
    m_tclInterp = Tcl_CreateInterp();
    if (m_tclInterp) {
        if (Tcl_Init(m_tclInterp) != TCL_OK) {
            EssConsoleManager::instance()->logError(
                QString("Tcl initialization failed: %1").arg(Tcl_GetStringResult(m_tclInterp)), 
                "CommandInterface"
            );
            Tcl_DeleteInterp(m_tclInterp);
            m_tclInterp = nullptr;
        } else {
            EssConsoleManager::instance()->logSuccess("Local Tcl interpreter initialized", "CommandInterface");
            
            // Set up some basic Tcl procedures
            const char* initScript = R"(
                proc help {} {
                    return "Available command channels:
  Local Tcl: Direct Tcl commands (default for unrecognized commands)
  ESS (port 2560): ::ess::* commands for experiment control
  dserv (port 4620): %* commands for datapoint access
  
Type 'info commands' to see all Tcl commands
Type 'ess help' to see ESS commands
Type 'dserv help' to see dserv commands"
                }
                
                proc ess {args} {
                    if {[llength $args] == 0 || [lindex $args 0] == "help"} {
                        return "ESS Commands (via port 2560):
  ::ess::load_system <s> <protocol> <variant>
  ::ess::reload_system
  ::ess::start / ::ess::stop / ::ess::reset
  ::ess::save_script <type> <content>
  ::ess::set_param <n> <value>
  ::ess::get_param <n>
  ::ess::list_systems / list_protocols / list_variants
  ::ess::get_status / get_system / get_protocol / get_variant"
                    }
                    return "Use full ::ess:: prefix for ESS commands"
                }
                
                proc dserv {args} {
                    if {[llength $args] == 0 || [lindex $args 0] == "help"} {
                        return "dserv Commands (via port 4620):
  %get <key> - Get datapoint value
  %set <key> <value> - Set datapoint value
  %getkeys [pattern] - List datapoint keys
  %match <ip> <port> <pattern> [every] - Subscribe to pattern
  %unmatch <ip> <port> <pattern> - Unsubscribe from pattern
  %touch <var> - Touch a variable
  %reg <ip> <port> <type> - Register listener
  %unreg <ip> <port> - Unregister listener"
                    }
                    return "Use % prefix for dserv commands"
                }
            )";
            
            if (Tcl_Eval(m_tclInterp, initScript) != TCL_OK) {
                EssConsoleManager::instance()->logWarning(
                    QString("Failed to set up help commands: %1").arg(Tcl_GetStringResult(m_tclInterp)),
                    "CommandInterface"
                );
            }
        }
    }
}

void EssCommandInterface::shutdownTcl()
{
    if (m_tclInterp) {
        Tcl_DeleteInterp(m_tclInterp);
        m_tclInterp = nullptr;
    }
}

bool EssCommandInterface::connectToHost(const QString &host)
{
    if (m_isConnected && m_currentHost == host) {
        return true; // Already connected
    }
    
    disconnectFromHost();
    
    EssConsoleManager::instance()->logInfo(
        QString("Connecting to %1...").arg(host), 
        "CommandInterface"
    );
    
    // Try to connect EssClient
    bool essConnected = m_essClient->connectToHost(host, 2560);
    if (essConnected) {
        EssConsoleManager::instance()->logSuccess(
            QString("Connected to ESS on %1:2560").arg(host), 
            "CommandInterface"
        );
    } else {
        EssConsoleManager::instance()->logWarning(
            QString("Failed to connect to ESS on %1:2560").arg(host), 
            "CommandInterface"
        );
    }
    
    // Test dserv connection with a simple command
    QString response;
    bool dservConnected = m_dservClient->sendCommand(host, 4620, "%getkeys", response);
    if (dservConnected) {
        EssConsoleManager::instance()->logSuccess(
            QString("Connected to dserv on %1:4620").arg(host), 
            "CommandInterface"
        );
    } else {
        EssConsoleManager::instance()->logWarning(
            QString("Failed to connect to dserv on %1:4620").arg(host), 
            "CommandInterface"
        );
    }
    
    m_isConnected = essConnected || dservConnected;
    m_currentHost = host;
    
    if (m_isConnected) {
        // Start listener if dserv is connected
        if (dservConnected && startListener()) {
            EssConsoleManager::instance()->logSuccess(
                QString("Listener started on port %1").arg(listenerPort()), 
                "CommandInterface"
            );
        }
        
        emit connected(host);
    } else {
        emit connectionError("Failed to connect to any service");
    }
    
    return m_isConnected;
}

void EssCommandInterface::disconnectFromHost()
{
    if (!m_isConnected) {
        return;
    }
    
    // Clear subscriptions
    clearSubscriptions();
    
    // Stop listener
    stopListener();
    
    // Reset and recreate clients
    m_dservClient.reset();
    m_essClient.reset();
    
    // IMPORTANT: Recreate the clients immediately!
    m_dservClient = std::make_unique<DservClient>(this);
    m_essClient = std::make_unique<EssClient>(this);
    
    QString oldHost = m_currentHost;
    m_isConnected = false;
    m_currentHost.clear();
    
    // Only log if we're not shutting down (check if QApplication still exists)
    if (QCoreApplication::instance()) {
        // Only try to log if console manager might still exist
        if (EssConsoleManager::instance()) {
            EssConsoleManager::instance()->logInfo(
                QString("Disconnected from %1").arg(oldHost), "Connection");
        }
    }
    
    emit disconnected();
}

bool EssCommandInterface::isConnected() const
{
    return m_isConnected;
}

EssCommandInterface::CommandChannel EssCommandInterface::detectChannel(const QString &command) const
{
    QString trimmed = command.trimmed();
    
    // Check for explicit channel prefixes
    if (trimmed.startsWith("local:") || trimmed.startsWith("tcl:")) {
        return ChannelLocal;
    }
    if (trimmed.startsWith("ess:")) {
        return ChannelEss;
    }
    if (trimmed.startsWith("dserv:")) {
        return ChannelDserv;
    }
    
    // Check for dserv commands (start with %)
    if (trimmed.startsWith("%")) {
        return ChannelDserv;
    }
    
    // Check for ESS commands
    for (const QString &essCmd : s_essCommands) {
        if (trimmed.startsWith(essCmd)) {
            return ChannelEss;
        }
    }
    
    // Default to local Tcl
    return ChannelLocal;
}

EssCommandInterface::CommandResult EssCommandInterface::executeCommand(const QString &command, CommandChannel channel)
{
    QString trimmedCommand = command.trimmed();
    if (trimmedCommand.isEmpty()) {
        return {StatusSuccess, "", "", channel};
    }
    
    // Use the specified channel or the default channel
    if (channel == ChannelAuto) {
        channel = m_defaultChannel;
    }
    
    // If still auto, default to local Tcl
    if (channel == ChannelAuto) {
        channel = ChannelLocal;
    }
    
    CommandResult result;
    result.channel = channel;
    
    switch (channel) {
        case ChannelLocal:
            result = executeLocalTcl(trimmedCommand);
            break;
            
        case ChannelEss:
            result = executeEss(trimmedCommand);
            break;
            
        case ChannelDserv:
            result = executeDserv(trimmedCommand);
            break;
            
        default:
            result.status = StatusError;
            result.error = "Unknown command channel";
    }
    
    return result;
}

void EssCommandInterface::executeCommandAsync(const QString &command, CommandChannel channel)
{
    // For now, just execute synchronously and emit result
    // Could be extended to use QThread for truly async execution
    CommandResult result = executeCommand(command, channel);
    emit commandCompleted(result);
}

EssCommandInterface::CommandResult EssCommandInterface::executeLocalTcl(const QString &command)
{
    CommandResult result;
    result.channel = ChannelLocal;
    
    if (!m_tclInterp) {
        result.status = StatusError;
        result.error = "Tcl interpreter not initialized";
        return result;
    }
    
    int tclResult = Tcl_Eval(m_tclInterp, command.toUtf8().constData());
    
    if (tclResult == TCL_OK) {
        result.status = StatusSuccess;
        result.response = QString::fromUtf8(Tcl_GetStringResult(m_tclInterp));
    } else {
        result.status = StatusError;
        result.error = QString::fromUtf8(Tcl_GetStringResult(m_tclInterp));
    }
    
    return result;
}

EssCommandInterface::CommandResult EssCommandInterface::executeEss(const QString &command)
{
    CommandResult result;
    result.channel = ChannelEss;
    
    if (!m_essClient->isConnected()) {
        result.status = StatusNotConnected;
        result.error = "Not connected to ESS service";
        return result;
    }
    
    QString response;
    bool success = m_essClient->sendCommand(command, response, 5000);
    
    if (success) {
        result.status = StatusSuccess;
        result.response = response;
    } else {
        result.status = StatusError;
        result.error = "Failed to execute ESS command";
    }
    
    return result;
}

EssCommandInterface::CommandResult EssCommandInterface::executeDserv(const QString &command)
{
    CommandResult result;
    result.channel = ChannelDserv;
    
    if (m_currentHost.isEmpty()) {
        result.status = StatusNotConnected;
        result.error = "Not connected to dserv";
        return result;
    }
    
    QString response;
    bool success = m_dservClient->sendCommand(m_currentHost, 4620, command, response);
    
    if (success) {
        // The raw response format from dserv is "STATUS DATA"
        // STATUS: 1 = success, 0 = not found, -1 = error
        int spaceIndex = response.indexOf(' ');
        if (spaceIndex > 0) {
            bool ok;
            int status = response.left(spaceIndex).toInt(&ok);
            if (ok) {
                if (status == 1) {
                    result.status = StatusSuccess;
                    result.response = response.mid(spaceIndex + 1);
                } else if (status == 0) {
                    result.status = StatusSuccess;
                    result.response = ""; // Not found but still successful
                } else {
                    result.status = StatusError;
                    result.error = QString("Server error (status %1)").arg(status);
                    if (spaceIndex < response.length() - 1) {
                        result.error += ": " + response.mid(spaceIndex + 1);
                    }
                }
            } else {
                // Couldn't parse status, return raw response
                result.status = StatusSuccess;
                result.response = response;
            }
        } else {
            // No space found - might be just a status code or simple response
            bool ok;
            int status = response.toInt(&ok);
            if (ok && (status == 0 || status == 1)) {
                result.status = StatusSuccess;
                result.response = "";
            } else {
                // Treat as raw response
                result.status = StatusSuccess;
                result.response = response;
            }
        }
    } else {
        result.status = StatusError;
        result.error = m_dservClient->lastError();
        if (result.error.isEmpty()) {
            result.error = "Failed to execute dserv command";
        }
    }
    
    return result;
}

QStringList EssCommandInterface::getAvailableCommands() const
{
    QStringList commands;
    
    // Add ESS commands
    commands.append(s_essCommands);
    
    // Add dserv commands
    commands.append(s_dservCommands);
    
    // Add Tcl commands
    commands.append(getTclCommands());
    
    return commands;
}

QStringList EssCommandInterface::getTclCommands() const
{
    QStringList commands;
    
    if (m_tclInterp) {
        // Get all Tcl commands
        int result = Tcl_Eval(m_tclInterp, "info commands");
        if (result == TCL_OK) {
            QString cmdList = QString::fromUtf8(Tcl_GetStringResult(m_tclInterp));
            commands = cmdList.split(' ', Qt::SkipEmptyParts);
        }
    }
    
    return commands;
}

QString EssCommandInterface::channelName(CommandChannel channel) const
{
    switch (channel) {
        case ChannelLocal: return "Local Tcl";
        case ChannelEss: return "ESS";
        case ChannelDserv: return "dserv";
        case ChannelAuto: return "Auto";
        default: return "Unknown";
    }
}

bool EssCommandInterface::startListener()
{
    if (!m_dservListener->startListening()) {
        EssConsoleManager::instance()->logError("Failed to start listener", "CommandInterface");
        return false;
    }
    
    quint16 port = m_dservListener->port();
    
    // Register listener with dserv
    if (!m_dservClient->registerListener(m_currentHost, port)) {
        EssConsoleManager::instance()->logError("Failed to register listener with dserv", "CommandInterface");
        m_dservListener->shutdown();
        return false;
    }
    
    return true;
}

void EssCommandInterface::stopListener()
{
    if (m_dservListener) {
        // First check if we need to unregister
        if (m_dservListener->isListening() && !m_currentHost.isEmpty()) {
            quint16 port = m_dservListener->port();
            // Try to unregister, but don't worry if it fails
            m_dservClient->unregisterListener(m_currentHost, port);
        }
        
        m_dservListener->shutdown();
        m_dservListener.reset();
        
        // Recreate the listener
        m_dservListener = std::make_unique<DservListener>(this);
        
        // Reconnect the signal
        connect(m_dservListener.get(), &DservListener::receivedEvent,
                this, &EssCommandInterface::onEventReceived);
    }
}

bool EssCommandInterface::isListening() const
{
    return m_dservListener->isListening();
}

quint16 EssCommandInterface::listenerPort() const
{
    return m_dservListener->port();
}

bool EssCommandInterface::subscribe(const QString &pattern, int every)
{
    if (!isListening()) {
        EssConsoleManager::instance()->logError("Listener not running", "CommandInterface");
        return false;
    }
    
    quint16 port = m_dservListener->port();
    
    if (m_dservClient->subscribeMatch(m_currentHost, port, pattern, every)) {
        m_activeSubscriptions.append(pattern);
        EssConsoleManager::instance()->logSuccess(
            QString("Subscribed to pattern: %1").arg(pattern), 
            "CommandInterface"
        );
        return true;
    } else {
        EssConsoleManager::instance()->logError(
            QString("Failed to subscribe to pattern: %1").arg(pattern), 
            "CommandInterface"
        );
        return false;
    }
}

bool EssCommandInterface::unsubscribe(const QString &pattern)
{
    if (!isListening()) {
        return false;
    }
    
    quint16 port = m_dservListener->port();
    
    if (m_dservClient->removeMatch(m_currentHost, port, pattern)) {
        m_activeSubscriptions.removeAll(pattern);
        EssConsoleManager::instance()->logInfo(
            QString("Unsubscribed from pattern: %1").arg(pattern), 
            "CommandInterface"
        );
        return true;
    }
    
    return false;
}

void EssCommandInterface::clearSubscriptions()
{
    if (!isListening()) return;
    
    quint16 port = m_dservListener->port();
    
    for (const QString &pattern : m_activeSubscriptions) {
        m_dservClient->removeMatch(m_currentHost, port, pattern);
    }
    m_activeSubscriptions.clear();
    
    EssConsoleManager::instance()->logInfo("Cleared all subscriptions", "CommandInterface");
}

// In EssCommandInterface::onEventReceived, update the logging:
void EssCommandInterface::onEventReceived(const QString &event)
{
    // Parse the event using DservEventParser
    auto parsedEvent = m_eventParser->parse(event);
    
    if (parsedEvent.has_value()) {
        const DservEvent &evt = parsedEvent.value();
        
        // Emit the parsed event
        emit datapointUpdated(evt.name, evt.data, evt.timestamp);
        
        // Special logging for eventlog/events
        if (evt.name == "eventlog/events" && evt.data.userType() == QMetaType::QVariantMap) {
            QVariantMap eventMap = evt.data.toMap();
            if (eventMap.contains("e_type") && eventMap.contains("e_subtype")) {
                uint8_t type = eventMap["e_type"].toUInt();
                uint8_t subtype = eventMap["e_subtype"].toUInt();
                QString params = eventMap.value("e_params", "").toString();
                
                EssConsoleManager::instance()->logInfo(
                    QString("Event[%1:%2] %3").arg(type).arg(subtype).arg(params), 
                    "Event"
                );
            } else {
                EssConsoleManager::instance()->logInfo(
                    QString("Datapoint: %1 = [Event Data]").arg(evt.name), 
                    "Listener"
                );
            }
        } else if (evt.name.contains("eye") || evt.name.contains("ain")) {
            EssConsoleManager::instance()->logDebug(
                QString("Datapoint: %1 = %2").arg(evt.name, evt.data.toString()), 
                "Listener"
            );
        } else {
            // For other datapoints, show the value
            QString valueStr;
            if (evt.data.userType() == QMetaType::QVariantMap) {
                valueStr = "[Map Data]";
            } else {
                valueStr = evt.data.toString();
            }
            
            EssConsoleManager::instance()->logInfo(
                QString("Datapoint: %1 = %2").arg(evt.name, valueStr), 
                "Listener"
            );
        }
    } else {
        qDebug() << "Failed to parse event";
        EssConsoleManager::instance()->logWarning(
            QString("Failed to parse event: %1").arg(event), 
            "Listener"
        );
    }
}

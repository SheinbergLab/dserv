#include "EssCommandInterface.h"
#include "EssApplication.h"
#include "core/EssDataProcessor.h"
#include "communication/DservClient.h"
#include "communication/EssClient.h"
#include "communication/DservListener.h"
#include "communication/DservEventParser.h"
#include "console/EssOutputConsole.h"

#include <QDebug>
#include <QMetaObject>
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
    }
    
    // Shutdown Tcl
    shutdownTcl();
}

DYN_GROUP* EssCommandInterface::getDynGroup(const QString &name) const
{
    if (!m_tclInterp) return nullptr;
    
    DYN_GROUP* dg = nullptr;
    int result = tclFindDynGroup(m_tclInterp, 
                                const_cast<char*>(name.toUtf8().constData()), 
                                &dg);
    
    if (result == TCL_OK) {
        return dg;
    }
    
    return nullptr;
}

QStringList EssCommandInterface::getDynGroupNames() const
{
    QStringList names;
    
    if (!m_tclInterp) return names;
    
    // Use Tcl to get the list of DG names
    int result = Tcl_Eval(m_tclInterp, "dgGetNames");
    if (result == TCL_OK) {
        QString nameList = QString::fromUtf8(Tcl_GetStringResult(m_tclInterp));
        names = nameList.split(' ', Qt::SkipEmptyParts);
    }
    
    return names;
}

QStringList EssCommandInterface::getDynListNames(const QString &groupName) const
{
    QStringList names;
    
    if (!m_tclInterp || groupName.isEmpty()) return names;
    
    // Use Tcl to get the list names
    QString cmd = QString("dgGetListNames %1").arg(groupName);
    int result = Tcl_Eval(m_tclInterp, cmd.toUtf8().constData());
    if (result == TCL_OK) {
        QString nameList = QString::fromUtf8(Tcl_GetStringResult(m_tclInterp));
        names = nameList.split(' ', Qt::SkipEmptyParts);
    }
    
    return names;
}

void EssCommandInterface::initializeTcl()
{
    m_tclInterp = Tcl_CreateInterp();
    if (!m_tclInterp) {
        EssConsoleManager::instance()->logError("Failed to create Tcl interpreter", "CommandInterface");
        return;
    }
    
    if (Tcl_Init(m_tclInterp) != TCL_OK) {
        EssConsoleManager::instance()->logError(
            QString("Tcl initialization failed: %1").arg(Tcl_GetStringResult(m_tclInterp)), 
            "CommandInterface"
        );
        Tcl_DeleteInterp(m_tclInterp);
        m_tclInterp = nullptr;
        return;
    }
    
    EssConsoleManager::instance()->logSuccess("Local Tcl interpreter initialized", "CommandInterface");
    
    // Register C++ commands with Tcl
    registerTclCommands();

   // Set up initialization script with procedures only
    const char* initScript = R"tcl(
        # Aliases for common variations
        interp alias {} quit {} exit
        interp alias {} ? {} help
        
        # Set initial channel
        set ess_channel "local"
        
        # Procedure to show current channel
        proc channel {} {
            global ess_channel
            return "Current channel: $ess_channel"
        }
        
        # Standard connection setup procedure
        proc setup_connection {host} {
            puts "Setting up connection to $host..."
            
            # Subscribe to essential datapoints
            set subscriptions {
                "ess/*"
                "system/*"
                "stimdg"
                "trialdg"
                "eventlog/events"
                "print"
            }
            
            foreach pattern $subscriptions {
                if {[catch {subscribe $pattern} err]} {
                    puts "Warning: Failed to subscribe to $pattern: $err"
                }
            }
            
            # Touch variables to initialize UI
            set touch_vars {
                ess/systems ess/protocols ess/variants
                ess/system ess/protocol ess/variant  
                ess/subject ess/state ess/obs_id ess/obs_total
                ess/block_pct_complete ess/block_pct_correct
                ess/variant_info_json ess/param_settings
                ess/system_script ess/protocol_script ess/variants_script
                ess/loaders_script ess/stim_script
                ess/state_table ess/rmt_cmds
                stimdg trialdg
                system/hostname system/os
            }
            
            # Touch all variables via ESS
            if {[catch {ess "foreach v {$touch_vars} { dservTouch \$v }"} err]} {
                puts "Warning: Failed to touch variables: $err"
            }
        }
        
        # Helper procedures  
        proc update_em_regions {} {
            ess {for {set i 0} {$i < 8} {incr i} {ainGetRegionInfo $i}}
        }
        
        proc update_touch_regions {} {
            ess {for {set i 0} {$i < 8} {incr i} {touchGetRegionInfo $i}}
        }
        
        proc load_dlsh {} {
            set f [file dirname [info nameofexecutable]]
            if { [file exists [file join $f dlsh.zip]] } { 
            	set dlshzip [file join $f dlsh.zip] 
            } else {
            	set dlshzip /usr/local/dlsh/dlsh.zip
            }
            set dlshroot [file join [zipfs root] dlsh]
            zipfs unmount $dlshroot
            zipfs mount $dlshzip $dlshroot
            set ::auto_path [linsert $::auto_path 0 [file join $dlshroot/lib]]
            package require dlsh
			package require qtcgwin
        }
        
        load_dlsh
    )tcl";
    
    if (Tcl_Eval(m_tclInterp, initScript) != TCL_OK) {
        EssConsoleManager::instance()->logWarning(
            QString("Failed to set up init script: %1").arg(Tcl_GetStringResult(m_tclInterp)),
            "CommandInterface"
        );
    }
    
    emit tclInitialized();
}    

void EssCommandInterface::checkPackagesAndEmit()
{
    if (!m_tclInterp) return;
    
    // Check if packages are loaded
    if (Tcl_Eval(m_tclInterp, "package present qtcgwin") == TCL_OK) {
        EssConsoleManager::instance()->logSuccess(
            "Packages confirmed loaded, emitting signal", "CommandInterface");
        emit packagesLoaded();
    } else {
        EssConsoleManager::instance()->logWarning(
            "Packages not yet loaded", "CommandInterface");
    }
}


void EssCommandInterface::registerTclCommands()
{
    // Connection commands
    Tcl_CreateObjCommand(m_tclInterp, "connect", TclConnectCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "disconnect", TclDisconnectCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "status", TclStatusCmd, this, nullptr);
    
    // Subscription commands
    Tcl_CreateObjCommand(m_tclInterp, "subscribe", TclSubscribeCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "unsubscribe", TclUnsubscribeCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "subscriptions", TclSubscriptionsCmd, this, nullptr);
    
    // UI commands
    Tcl_CreateObjCommand(m_tclInterp, "clear", TclClearCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "about", TclAboutCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "help", TclHelpCmd, this, nullptr);
    
    // Backend command proxies
    Tcl_CreateObjCommand(m_tclInterp, "ess", TclEssCmd, this, nullptr);
    Tcl_CreateObjCommand(m_tclInterp, "dserv", TclDservCmd, this, nullptr);
}

void EssCommandInterface::shutdownTcl()
{
    if (m_tclInterp) {
        Tcl_DeleteInterp(m_tclInterp);
        m_tclInterp = nullptr;
    }
}

// Helper to emit Qt signals from static Tcl callbacks
void EssCommandInterface::emitSignal(EssCommandInterface *obj, const char *signal)
{
    // Use QMetaObject to safely emit signals from static context
    QMetaObject::invokeMethod(obj, signal, Qt::QueuedConnection);
}

EssCommandInterface::CommandResult EssCommandInterface::executeCommand(const QString &command, CommandChannel channel)
{
    QString trimmedCommand = command.trimmed();
    CommandResult result;
    
    if (trimmedCommand.isEmpty()) {
        result.status = StatusSuccess;
        return result;
    }
    
    // Handle channel switches (these are special and don't go to Tcl)
    if (trimmedCommand == "/local" || trimmedCommand == "/tcl") {
        setDefaultChannel(ChannelLocal);
        Tcl_SetVar(m_tclInterp, "ess_channel", "local", TCL_GLOBAL_ONLY);
        result.status = StatusSuccess;
        result.response = "Switched to Local Tcl channel";
        result.channel = ChannelLocal;
        return result;
    } else if (trimmedCommand == "/ess") {
        setDefaultChannel(ChannelEss);
        Tcl_SetVar(m_tclInterp, "ess_channel", "ess", TCL_GLOBAL_ONLY);
        result.status = StatusSuccess;
        result.response = "Switched to ESS channel (port 2560)";
        result.channel = ChannelEss;
        return result;
    } else if (trimmedCommand == "/dserv") {
        setDefaultChannel(ChannelDserv);
        Tcl_SetVar(m_tclInterp, "ess_channel", "dserv", TCL_GLOBAL_ONLY);
        result.status = StatusSuccess;
        result.response = "Switched to dserv channel (port 4620)";
        result.channel = ChannelDserv;
        return result;
    }
    
    // Special handling for exit (emit signal but also return success)
    if (trimmedCommand == "exit" || trimmedCommand == "quit") {
      //        emitSignal(this, "quitRequested");
      result.status = StatusSuccess;
      result.response = "";
        return result;
    }
    
    // Route based on current channel
    if (m_defaultChannel == ChannelEss) {
        // In ESS mode, send directly to ESS backend
        return executeEss(trimmedCommand);
    } else if (m_defaultChannel == ChannelDserv) {
        // In dserv mode, send directly to dserv backend
        return executeDserv(trimmedCommand);
    } else {
        // In local mode, everything goes through Tcl
        return executeLocalTcl(trimmedCommand);
    }
}

// Tcl command implementations
// All Tcl command implementations simplified - no QMetaObject needed

int EssCommandInterface::TclConnectCmd(ClientData clientData, Tcl_Interp *interp, 
                                       int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "hostname");
        return TCL_ERROR;
    }
    
    QString host = Tcl_GetString(objv[1]);
    
    if (self->connectToHost(host)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Connected", -1));
        return TCL_OK;
    } else {
        Tcl_SetResult(interp, "Connection failed", TCL_STATIC);
        return TCL_ERROR;
    }
}

void EssCommandInterface::requestDisconnect()
{
    // Emit the signal to let the application handle the request
    emit disconnectRequested();
}

// Also update the TclDisconnectCmd to use the new approach:
int EssCommandInterface::TclDisconnectCmd(ClientData clientData, Tcl_Interp *interp,
                                         int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    // Get current host before disconnecting for cleanup
    QString currentHost = self->currentHost();
    
    // Call cleanup procedure if connected
    if (!currentHost.isEmpty()) {
        QString cleanupCmd = QString("if {[info procs cleanup_connection] ne \"\"} { cleanup_connection {%1} }").arg(currentHost);
        Tcl_Eval(interp, cleanupCmd.toUtf8().constData());
    }
    
    // Request disconnect through the signal mechanism
    self->requestDisconnect();
    return TCL_OK;
}

int EssCommandInterface::TclSubscribeCmd(ClientData clientData, Tcl_Interp *interp,
                                        int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "pattern ?every?");
        return TCL_ERROR;
    }
    
    QString pattern = Tcl_GetString(objv[1]);
    int every = 1;
    
    if (objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    
    if (!self->isConnected()) {
        Tcl_SetResult(interp, "Not connected", TCL_STATIC);
        return TCL_ERROR;
    }
    
    if (self->subscribe(pattern, every)) {
        return TCL_OK;
    } else {
        Tcl_SetResult(interp, "Subscribe failed", TCL_STATIC);
        return TCL_ERROR;
    }
}

int EssCommandInterface::TclUnsubscribeCmd(ClientData clientData, Tcl_Interp *interp,
                                          int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "pattern");
        return TCL_ERROR;
    }
    
    QString pattern = Tcl_GetString(objv[1]);
    
    if (self->unsubscribe(pattern)) {
        return TCL_OK;
    } else {
        Tcl_SetResult(interp, "Unsubscribe failed", TCL_STATIC);
        return TCL_ERROR;
    }
}

int EssCommandInterface::TclSubscriptionsCmd(ClientData clientData, Tcl_Interp *interp,
                                            int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    QStringList subs = self->activeSubscriptions();
    
    if (subs.isEmpty()) {
        Tcl_SetResult(interp, "No active subscriptions", TCL_STATIC);
    } else {
        QString result = "Active subscriptions:\n";
        for (const QString &sub : subs) {
            result += "  " + sub + "\n";
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(result.toUtf8().data(), -1));
    }
    
    return TCL_OK;
}

int EssCommandInterface::TclStatusCmd(ClientData clientData, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    QString status;
    if (self->isConnected()) {
        status = QString("Connected to %1").arg(self->currentHost());
        
        QStringList subs = self->activeSubscriptions();
        if (!subs.isEmpty()) {
            status += QString("\nActive subscriptions: %1").arg(subs.count());
        }
    } else {
        status = "Not connected";
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(status.toUtf8().data(), -1));
    return TCL_OK;
}

int EssCommandInterface::TclClearCmd(ClientData clientData, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    emit self->clearRequested();
    return TCL_OK;
}

int EssCommandInterface::TclAboutCmd(ClientData clientData, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    emit self->aboutRequested();
    return TCL_OK;
}

int EssCommandInterface::TclHelpCmd(ClientData clientData, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *const objv[])
{
    const char *helpText = R"(ESS Qt Terminal Commands
========================

Connection Commands:
  connect <host>      - Connect to ESS/dserv host
  disconnect          - Disconnect from current host
  status              - Show connection status

Subscription Commands:
  subscribe <pattern> ?every?  - Subscribe to datapoint pattern
  unsubscribe <pattern>        - Unsubscribe from pattern
  subscriptions               - List active subscriptions

UI Commands:
  clear               - Clear terminal
  about               - Show about dialog
  help                - Show this help
  exit/quit           - Exit application

Backend Commands:
  ess <command>       - Send command to ESS (port 2560)
  dserv <command>     - Send command to dserv (port 4620)

Channel Switching:
  /local or /tcl      - Switch to local Tcl mode
  /ess                - Switch to ESS mode
  /dserv              - Switch to dserv mode

Examples:
  connect localhost
  subscribe "ain/*"
  ess get_status
  dserv %getkeys
  
You can also use any Tcl command in local mode.)";
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(helpText, -1));
    
    // Also emit signal for other widgets if needed
    auto *self = static_cast<EssCommandInterface*>(clientData);
    emit self->helpRequested(QString(helpText));
    
    return TCL_OK;
}

int EssCommandInterface::TclEssCmd(ClientData clientData, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    if (objc < 2) {
        Tcl_SetResult(interp, "Usage: ess command ?args ...?", TCL_STATIC);
        return TCL_ERROR;
    }
    
    // Reconstruct the ESS command
    QString essCommand;
    for (int i = 1; i < objc; ++i) {
        if (i > 1) essCommand += " ";
        essCommand += Tcl_GetString(objv[i]);
    }
    
    CommandResult result = self->executeEss(essCommand);
    
    if (result.status == StatusSuccess) {
        if (!result.response.isEmpty()) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(result.response.toUtf8().data(), -1));
        }
        return TCL_OK;
    } else {
        QString error = result.error.isEmpty() ? "ESS command failed" : result.error;
        Tcl_SetResult(interp, error.toUtf8().data(), TCL_VOLATILE);
        return TCL_ERROR;
    }
}

int EssCommandInterface::TclDservCmd(ClientData clientData, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *const objv[])
{
    auto *self = static_cast<EssCommandInterface*>(clientData);
    
    if (objc < 2) {
        Tcl_SetResult(interp, "Usage: dserv command ?args ...?", TCL_STATIC);
        return TCL_ERROR;
    }
    
    // Reconstruct the dserv command with % prefix if not present
    QString dservCommand;
    QString firstArg = Tcl_GetString(objv[1]);
    if (!firstArg.startsWith("%")) {
        dservCommand = "%";
    }
    
    for (int i = 1; i < objc; ++i) {
        if (i > 1) dservCommand += " ";
        dservCommand += Tcl_GetString(objv[i]);
    }
    
    CommandResult result = self->executeDserv(dservCommand);
    
    if (result.status == StatusSuccess) {
        if (!result.response.isEmpty()) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(result.response.toUtf8().data(), -1));
        }
        return TCL_OK;
    } else {
        QString error = result.error.isEmpty() ? "dserv command failed" : result.error;
        Tcl_SetResult(interp, error.toUtf8().data(), TCL_VOLATILE);
        return TCL_ERROR;
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
        
        // Call the Tcl setup procedure if it exists
        QString setupCmd = QString("if {[info procs setup_connection] ne \"\"} { setup_connection {%1} }").arg(host);
        int result = Tcl_Eval(m_tclInterp, setupCmd.toUtf8().constData());
        
        if (result != TCL_OK) {
            EssConsoleManager::instance()->logWarning(
                QString("Connection setup script failed: %1").arg(Tcl_GetStringResult(m_tclInterp)),
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
    
    // Stop listener first - this will unregister and automatically clear all matches
    stopListener();
    
    // Clear our local subscription list without trying to remove from server
    m_activeSubscriptions.clear();
    
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

EssCommandInterface::CommandResult EssCommandInterface::executeEssAsync(const QString &command)
{
    CommandResult result;
    result.channel = ChannelEss;
    
    if (!m_essClient->isConnected()) {
        result.status = StatusNotConnected;
        result.error = "Not connected to ESS service";
        return result;
    }
    
    QString response;
    bool success = m_essClient->sendAsyncCommand(command, response, 5000);
    
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
    if (!isListening() || m_currentHost.isEmpty()) {
        // If not listening or no host, just clear the list
        m_activeSubscriptions.clear();
        return;
    }
    
    quint16 port = m_dservListener->port();
    
    for (const QString &pattern : m_activeSubscriptions) {
        // During shutdown, these might fail if listener was already unregistered
        m_dservClient->removeMatch(m_currentHost, port, pattern);
    }
    m_activeSubscriptions.clear();
    
    EssConsoleManager::instance()->logInfo("Cleared all subscriptions", "CommandInterface");
}

void EssCommandInterface::onEventReceived(const QString &event)
{
    // Parse the event using DservEventParser
    auto parsedEvent = m_eventParser->parse(event);
    
    if (parsedEvent.has_value()) {
        const DservEvent &evt = parsedEvent.value();
        
        // Pass dtype along with the data
        emit datapointUpdated(evt.name, evt.data, evt.timestamp, evt.dtype);
        
    } else {
        // Only log actual parse errors, not every event
        EssConsoleManager::instance()->logError(
            QString("Failed to parse event from listener"), 
            "CommandInterface"
        );
    }
}


// EssScriptableManager.cpp
#include "EssScriptableManager.h"
#include "EssScriptableWidget.h"
#include "EssApplication.h"
#include "EssStandaloneWindow.h"
#include "EssWorkspaceManager.h"
#include "EssMainWindow.h"

#include "console/EssOutputConsole.h"

#include <QDebug>


static int tcl_create_graphics_widget(ClientData clientData, 
	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static int tcl_cgraph_standalone(ClientData clientData, Tcl_Interp* interp,
                                int objc, Tcl_Obj* const objv[]);
static int tcl_list_graphics_widgets(ClientData clientData,
 	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static int tcl_send_to_graphics_widget(ClientData clientData, 
	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static int tcl_broadcast_to_graphics_widgets(ClientData clientData, 
	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static int tcl_list_graphics_templates(ClientData clientData, 
	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static int tcl_set_graphics_template(ClientData clientData, 
	Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
static void registerCgraphCommands(Tcl_Interp* interp);

EssScriptableManager& EssScriptableManager::getInstance()
{
    static EssScriptableManager instance;
    return instance;
}


QString EssScriptableManager::registerWidget(const QString& name, 
    EssScriptableWidget* widget)
{
    if (!widget) return QString();
    
    QString widgetName = name;
    
    m_widgets[widgetName] = widget;
    
    // Connect widget signals
    connectWidgetSignals(widget);
    
    // Connect to destroyed signal for auto-cleanup
    connect(widget, &QObject::destroyed, this, &EssScriptableManager::onWidgetDestroyed);
    
    emit widgetRegistered(widgetName, widget);
    
    EssConsoleManager::instance()->logInfo(
        QString("Scriptable widget registered: %1 (%2)").arg(widgetName, widget->getWidgetTypeName()),
        "ScriptableManager"
    );
    
    return widgetName;
}

bool EssScriptableManager::unregisterWidget(const QString& name)
{
    auto it = m_widgets.find(name);
    if (it == m_widgets.end()) {
        return false;
    }
    
    EssScriptableWidget* widget = it.value();
    
    // Disconnect signals
    if (widget) {
        disconnectWidgetSignals(widget);
    }
    
    // Remove from all groups
    for (auto& groupList : m_groups) {
        groupList.removeAll(name);
    }
    
    m_widgets.erase(it);
    emit widgetUnregistered(name);
    
    EssConsoleManager::instance()->logInfo(
        QString("Scriptable widget unregistered: %1").arg(name),
        "ScriptableManager"
    );
    
    return true;
}

EssScriptableWidget* EssScriptableManager::getWidget(const QString& name) const
{
    return m_widgets.value(name, nullptr);
}

QList<QString> EssScriptableManager::getAllWidgetNames() const
{
    return m_widgets.keys();
}

QList<EssScriptableWidget*> EssScriptableManager::getAllWidgets() const
{
    QList<EssScriptableWidget*> result;
    for (auto widget : m_widgets) {
        if (widget) {
            result.append(widget);
        }
    }
    return result;
}

int EssScriptableManager::send(const QString& widgetName, const QString& command)
{
    EssScriptableWidget* widget = getWidget(widgetName);
    if (!widget) {
        EssConsoleManager::instance()->logWarning(
            QString("Widget not found: %1").arg(widgetName),
            "ScriptableManager"
        );
        return TCL_ERROR;
    }
    
    int result = widget->eval(command);
    emit commandSent(widgetName, command, result);
    
    EssConsoleManager::instance()->logDebug(
        QString("Command sent to %1: %2 (result: %3)")
            .arg(widgetName, command.left(50), result == TCL_OK ? "OK" : "ERROR"),
        "ScriptableManager"
    );
    
    return result;
}

int EssScriptableManager::broadcast(const QString& command)
{
    int failures = 0;
    for (auto it = m_widgets.begin(); it != m_widgets.end(); ++it) {
        if (it.value()) {
            int result = it.value()->eval(command);
            emit commandSent(it.key(), command, result);
            if (result != TCL_OK) {
                failures++;
            }
        }
    }
    
    EssConsoleManager::instance()->logInfo(
        QString("Broadcast command to %1 widgets: %2 (failures: %3)")
            .arg(m_widgets.size()).arg(command.left(50)).arg(failures),
        "ScriptableManager"
    );
    
    return failures > 0 ? TCL_ERROR : TCL_OK;
}

int EssScriptableManager::sendToGroup(const QString& groupTag, const QString& command)
{
    QStringList members = m_groups.value(groupTag);
    if (members.isEmpty()) {
        EssConsoleManager::instance()->logWarning(
            QString("No widgets in group: %1").arg(groupTag),
            "ScriptableManager"
        );
        return TCL_ERROR;
    }
    
    int failures = 0;
    for (const QString& widgetName : members) {
        if (send(widgetName, command) != TCL_OK) {
            failures++;
        }
    }
    
    EssConsoleManager::instance()->logInfo(
        QString("Command sent to group '%1' (%2 widgets): %3 (failures: %4)")
            .arg(groupTag).arg(members.size()).arg(command.left(50)).arg(failures),
        "ScriptableManager"
    );
    
    return failures > 0 ? TCL_ERROR : TCL_OK;
}

int EssScriptableManager::sendToType(const QString& widgetType, const QString& command)
{
    QStringList widgets = getWidgetsByType(widgetType);
    if (widgets.isEmpty()) {
        EssConsoleManager::instance()->logWarning(
            QString("No widgets of type: %1").arg(widgetType),
            "ScriptableManager"
        );
        return TCL_ERROR;
    }
    
    int failures = 0;
    for (const QString& widgetName : widgets) {
        if (send(widgetName, command) != TCL_OK) {
            failures++;
        }
    }
    
    EssConsoleManager::instance()->logInfo(
        QString("Command sent to type '%1' (%2 widgets): %3 (failures: %4)")
            .arg(widgetType).arg(widgets.size()).arg(command.left(50)).arg(failures),
        "ScriptableManager"
    );
    
    return failures > 0 ? TCL_ERROR : TCL_OK;
}

void EssScriptableManager::addToGroup(const QString& widgetName, const QString& groupTag)
{
    if (!m_widgets.contains(widgetName)) {
        EssConsoleManager::instance()->logWarning(
            QString("Widget not found: %1").arg(widgetName),
            "ScriptableManager"
        );
        return;
    }
    
    if (!m_groups[groupTag].contains(widgetName)) {
        m_groups[groupTag].append(widgetName);
        emit groupModified(groupTag);
        
        EssConsoleManager::instance()->logDebug(
            QString("Widget '%1' added to group '%2'").arg(widgetName, groupTag),
            "ScriptableManager"
        );
    }
}

void EssScriptableManager::removeFromGroup(const QString& widgetName, const QString& groupTag)
{
    m_groups[groupTag].removeAll(widgetName);
    
    // Clean up empty groups
    if (m_groups[groupTag].isEmpty()) {
        m_groups.remove(groupTag);
    }
    
    emit groupModified(groupTag);
    
    EssConsoleManager::instance()->logDebug(
        QString("Widget '%1' removed from group '%2'").arg(widgetName, groupTag),
        "ScriptableManager"
    );
}

QStringList EssScriptableManager::getGroupMembers(const QString& groupTag) const
{
    return m_groups.value(groupTag);
}

QStringList EssScriptableManager::getAllGroups() const
{
    return m_groups.keys();
}

QStringList EssScriptableManager::getWidgetsByType(const QString& widgetType) const
{
    QStringList result;
    for (auto it = m_widgets.begin(); it != m_widgets.end(); ++it) {
        if (it.value() && it.value()->getWidgetTypeName() == widgetType) {
            result.append(it.key());
        }
    }
    return result;
}

QStringList EssScriptableManager::getAllTypes() const
{
    QStringList types;
    for (auto widget : m_widgets) {
        if (widget) {
            QString type = widget->getWidgetTypeName();
            if (!types.contains(type)) {
                types.append(type);
            }
        }
    }
    return types;
}

void EssScriptableManager::setSharedData(const QString& key, const QVariant& value)
{
    m_sharedData[key] = value;
    emit sharedDataChanged(key, value);
    
    EssConsoleManager::instance()->logDebug(
        QString("Shared data set: %1 = %2").arg(key, value.toString()),
        "ScriptableManager"
    );
}

QVariant EssScriptableManager::getSharedData(const QString& key) const
{
    return m_sharedData.value(key);
}

void EssScriptableManager::clearSharedData()
{
    m_sharedData.clear();
    EssConsoleManager::instance()->logInfo("Shared data cleared", "ScriptableManager");
}

void EssScriptableManager::setDevelopmentLayout(const QString& widgetName, int layoutMode)
{
    EssScriptableWidget* widget = getWidget(widgetName);
    if (widget) {
        widget->setDevelopmentLayout(static_cast<EssScriptableWidget::DevLayoutMode>(layoutMode));
    }
}

QString EssScriptableManager::getWidgetScript(const QString& widgetName) const
{
    EssScriptableWidget* widget = getWidget(widgetName);
    return widget ? widget->getSetupScript() : QString();
}

void EssScriptableManager::reloadAllScripts()
{
    for (auto widget : m_widgets) {
        if (widget) {
            widget->executeSetupScript();
        }
    }
    
    EssConsoleManager::instance()->logInfo("All widget scripts reloaded", "ScriptableManager");
}

bool EssScriptableManager::isWidgetNameAvailable(const QString& name)
{
    return !m_widgets.contains(name);
}

QString EssScriptableManager::createGraphicsWidget(const QString& name)
{
     if (!isWidgetNameAvailable(name)) {
        QString message = QString("Widget '%1' already exists (any type). Please choose a different name.").arg(name);
        // Show error and return empty string to indicate failure
        emit widgetCreationFailed(name, message);
        return QString();
    }
       
    // Emit simplified signal
    emit graphicsWidgetCreationRequested(name);  // Remove "basic" parameter
    
    EssConsoleManager::instance()->logInfo(
        QString("Graphics widget creation requested: %1").arg(name),
        "ScriptableManager"
    );
    
    return name;
}

bool EssScriptableManager::sendScriptToGraphicsWidget(const QString& widgetName, const QString& script)
{
    EssScriptableWidget* widget = getWidget(widgetName);
    if (!widget || widget->getWidgetTypeName() != "GraphicsWidget") {
        return false;
    }
    
    return send(widgetName, script) == TCL_OK;
}

int EssScriptableManager::broadcastToGraphicsWidgets(const QString& script)
{
    return sendToType("GraphicsWidget", script);
}

QStringList EssScriptableManager::getAllGraphicsWidgets() const
{
    return getWidgetsByType("GraphicsWidget");
}


void EssScriptableManager::connectWidgetSignals(EssScriptableWidget* widget)
{
    if (!widget) return;
    
    connect(widget, &EssScriptableWidget::scriptExecuted,
            this, &EssScriptableManager::onWidgetScriptExecuted);
}

void EssScriptableManager::disconnectWidgetSignals(EssScriptableWidget* widget)
{
    if (!widget) return;
    
    disconnect(widget, &EssScriptableWidget::scriptExecuted,
               this, &EssScriptableManager::onWidgetScriptExecuted);
}

void EssScriptableManager::onWidgetDestroyed(QObject* obj)
{
    // Find and remove the destroyed widget
    for (auto it = m_widgets.begin(); it != m_widgets.end(); ++it) {
        if (it.value() == obj) {
            QString name = it.key();
            unregisterWidget(name);
            break;
        }
    }
}

void EssScriptableManager::onWidgetScriptExecuted(int result, const QString& output)
{
    // Could log script execution results here if needed
    Q_UNUSED(result)
    Q_UNUSED(output)
}

// Tcl command bindings for main interpreter
void EssScriptableManager::registerTclCommands(Tcl_Interp* interp)
{
    if (!interp) return;
    
    // Core widget management commands
    Tcl_CreateObjCommand(interp, "scriptable_list", tcl_list_widgets, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_send", tcl_send_widget, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_broadcast", tcl_broadcast_widgets, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_send_to_type", tcl_send_to_type, nullptr, nullptr);
    
    // Group management commands
    Tcl_CreateObjCommand(interp, "scriptable_add_to_group", tcl_add_to_group, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_remove_from_group", tcl_remove_from_group, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_send_to_group", tcl_send_to_group, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_list_groups", tcl_list_groups, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_list_group_members", tcl_list_group_members, nullptr, nullptr);
    
    // Widget information commands
    Tcl_CreateObjCommand(interp, "scriptable_list_types", tcl_list_types, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_widget_type", tcl_get_widget_type, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_widget_exists", tcl_widget_exists, nullptr, nullptr);
    
    // Development commands
    Tcl_CreateObjCommand(interp, "scriptable_dev_mode", tcl_set_dev_mode, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_dev_layout", tcl_set_dev_layout, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_reload_scripts", tcl_reload_scripts, nullptr, nullptr);
    
    // Shared data commands
    Tcl_CreateObjCommand(interp, "scriptable_set_shared", tcl_set_shared_data, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_get_shared", tcl_get_shared_data, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "scriptable_clear_shared", tcl_clear_shared_data, nullptr, nullptr);
    
     registerCgraphCommands(interp);
     
    EssConsoleManager::instance()->logInfo("Scriptable manager Tcl commands registered", "ScriptableManager");
}

void EssScriptableManager::unregisterTclCommands(Tcl_Interp* interp)
{
    if (!interp) return;
    
    // Remove all commands
    const char* commands[] = {
        "scriptable_list", "scriptable_send", "scriptable_broadcast", "scriptable_send_to_type",
        "scriptable_add_to_group", "scriptable_remove_from_group", "scriptable_send_to_group", 
        "scriptable_list_groups", "scriptable_list_group_members",
        "scriptable_list_types", "scriptable_widget_type", "scriptable_widget_exists",
        "scriptable_dev_mode", "scriptable_dev_layout", "scriptable_reload_scripts",
        "scriptable_set_shared", "scriptable_get_shared", "scriptable_clear_shared"
    };
    
    for (const char* cmd : commands) {
        Tcl_DeleteCommand(interp, cmd);
    }
    
    EssConsoleManager::instance()->logInfo("Scriptable manager Tcl commands unregistered", "ScriptableManager");
}

// Static Tcl command implementations
int EssScriptableManager::tcl_list_widgets(ClientData clientData, Tcl_Interp* interp,
                                           int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QStringList names = manager.getAllWidgetNames();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& name : names) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(name.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

int EssScriptableManager::tcl_send_widget(ClientData clientData, Tcl_Interp* interp,
                                          int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name command");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString command = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    EssScriptableWidget* widget = manager.getWidget(widgetName);
    if (!widget) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Widget not found", -1));
        return TCL_ERROR;
    }
    
    int result = widget->eval(command);
    QString widgetResult = widget->result();
    
    // Set the result from the widget
    if (!widgetResult.isEmpty()) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(widgetResult.toUtf8().constData(), -1));
    }
    
    return result;
}

int EssScriptableManager::tcl_broadcast_widgets(ClientData clientData, Tcl_Interp* interp,
                                                int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString command = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    int result = manager.broadcast(command);
    
    if (result != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("One or more commands failed", -1));
    }
    
    return result;
}

int EssScriptableManager::tcl_send_to_type(ClientData clientData, Tcl_Interp* interp,
                                           int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_type command");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetType = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString command = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    int result = manager.sendToType(widgetType, command);
    
    if (result != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Type command failed", -1));
    }
    
    return result;
}

int EssScriptableManager::tcl_add_to_group(ClientData clientData, Tcl_Interp* interp,
                                           int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name group_name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString groupName = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    manager.addToGroup(widgetName, groupName);
    return TCL_OK;
}

int EssScriptableManager::tcl_send_to_group(ClientData clientData, Tcl_Interp* interp,
                                            int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "group_name command");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString groupName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString command = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    int result = manager.sendToGroup(groupName, command);
    
    if (result != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Group command failed", -1));
    }
    
    return result;
}

int EssScriptableManager::tcl_list_groups(ClientData clientData, Tcl_Interp* interp,
                                          int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QStringList groups = manager.getAllGroups();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& group : groups) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(group.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

int EssScriptableManager::tcl_set_shared_data(ClientData clientData, Tcl_Interp* interp,
                                              int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key value");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString key = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString value = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    manager.setSharedData(key, value);
    return TCL_OK;
}

int EssScriptableManager::tcl_get_shared_data(ClientData clientData, Tcl_Interp* interp,
                                              int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString key = QString::fromUtf8(Tcl_GetString(objv[1]));
    QVariant value = manager.getSharedData(key);
    
    if (value.isValid()) {
        QString strValue = value.toString();
        Tcl_SetObjResult(interp, Tcl_NewStringObj(strValue.toUtf8().constData(), -1));
    }
    
    return TCL_OK;
}


int EssScriptableManager::tcl_remove_from_group(ClientData clientData, Tcl_Interp* interp,
                                                int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name group_name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString groupName = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    manager.removeFromGroup(widgetName, groupName);
    return TCL_OK;
}

int EssScriptableManager::tcl_list_group_members(ClientData clientData, Tcl_Interp* interp,
                                                 int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "group_name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString groupName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QStringList members = manager.getGroupMembers(groupName);
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& member : members) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(member.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

int EssScriptableManager::tcl_list_types(ClientData clientData, Tcl_Interp* interp,
                                         int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QStringList types = manager.getAllTypes();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& type : types) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(type.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

int EssScriptableManager::tcl_get_widget_type(ClientData clientData, Tcl_Interp* interp,
                                              int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    EssScriptableWidget* widget = manager.getWidget(widgetName);
    
    if (!widget) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Widget not found", -1));
        return TCL_ERROR;
    }
    
    QString type = widget->widgetTypeName();
    Tcl_SetObjResult(interp, Tcl_NewStringObj(type.toUtf8().constData(), -1));
    return TCL_OK;
}

int EssScriptableManager::tcl_widget_exists(ClientData clientData, Tcl_Interp* interp,
                                            int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    bool exists = manager.getWidget(widgetName) != nullptr;
    
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(exists ? 1 : 0));
    return TCL_OK;
}

int EssScriptableManager::tcl_set_dev_mode(ClientData clientData, Tcl_Interp* interp,
                                           int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name enable");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    int enable;
    if (Tcl_GetBooleanFromObj(interp, objv[2], &enable) != TCL_OK) {
        return TCL_ERROR;
    }
    
    EssScriptableWidget* widget = manager.getWidget(widgetName);
    if (!widget) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Widget not found", -1));
        return TCL_ERROR;
    }
    
    widget->setDevelopmentMode(enable != 0);
    
    QString message = QString("Development mode %1 for widget '%2'")
                     .arg(enable ? "enabled" : "disabled")
                     .arg(widgetName);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message.toUtf8().constData(), -1));
    
    return TCL_OK;
}

int EssScriptableManager::tcl_set_dev_layout(ClientData clientData, Tcl_Interp* interp,
                                             int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name layout_mode");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    int layoutMode;
    if (Tcl_GetIntFromObj(interp, objv[2], &layoutMode) != TCL_OK) {
        return TCL_ERROR;
    }
    
    manager.setDevelopmentLayout(widgetName, layoutMode);
    return TCL_OK;
}

int EssScriptableManager::tcl_reload_scripts(ClientData clientData, Tcl_Interp* interp,
                                             int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    manager.reloadAllScripts();
    return TCL_OK;
}

int EssScriptableManager::tcl_clear_shared_data(ClientData clientData, Tcl_Interp* interp,
                                                int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    manager.clearSharedData();
    return TCL_OK;
}

static void registerCgraphCommands(Tcl_Interp* interp)
{
    // Graphics widget specific commands
    Tcl_CreateObjCommand(interp, "create_graphics_widget", tcl_create_graphics_widget, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "cgraph_standalone", tcl_cgraph_standalone, nullptr, nullptr);

    Tcl_CreateObjCommand(interp, "list_graphics_widgets", tcl_list_graphics_widgets, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "send_to_graphics_widget", tcl_send_to_graphics_widget, nullptr, nullptr);
    Tcl_CreateObjCommand(interp, "broadcast_to_graphics_widgets", tcl_broadcast_to_graphics_widgets, nullptr, nullptr);
    
    
    // Convenience script
    const char* graphicsScript = R"tcl(
        # Convenience aliases for graphics widget management
        proc cgraph {name} {
            create_graphics_widget $name
        }
        
        proc cglist {} {
            list_graphics_widgets
        }
        
        proc cgsend {widget script} {
            send_to_graphics_widget $widget $script
        }
        
        proc cgbroadcast {script} {
            broadcast_to_graphics_widgets $script
        }
        
        # Quick creation commands
        proc create_experiment_widget {name} {
            create_graphics_widget $name experiment
        }
        
        proc create_plot_widget {name} {
            create_graphics_widget $name dataplot
        }
        
        
        # Help command
        proc cghelp {} {
            puts "Graphics Widget Commands:"
            puts "  cgraph <name>                   - Create graphics widget"
            puts "  cglist                          - List all graphics widgets"
            puts "  cgsend <widget> <script>        - Send script to specific widget"
            puts "  cgbroadcast <script>            - Send script to all graphics widgets"
            puts ""
            puts "Quick creation:"
            puts "  create_experiment_widget <name> - Create experiment visualization"
            puts "  create_plot_widget <name>       - Create data plotting widget"
            puts ""
            puts "Examples:"
            puts "  cgraph myplot dataplot"
            puts "  cgsend myplot \"clearwin; setcolor red; circle 100 100 20 1\""
            puts "  cgbroadcast \"clearwin\""
        }
    )tcl";
    
    if (Tcl_Eval(interp, graphicsScript) != TCL_OK) {
        EssConsoleManager::instance()->logWarning(
            QString("Failed to set up graphics convenience commands: %1").arg(Tcl_GetStringResult(interp)),
            "ScriptableManager"
        );
    }
}

static int tcl_create_graphics_widget(ClientData clientData, Tcl_Interp* interp,
                                       int objc, Tcl_Obj* const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString name = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    // Check if widget already exists
    if (manager.getWidget(name)) {
        QString errorMsg = QString("widget '%1' already exists").arg(name);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(errorMsg.toUtf8().constData(), -1));
        return TCL_ERROR;
    }
    
    QString actualName = manager.createGraphicsWidget(name);
    
    if (actualName.isEmpty()) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Failed to create graphics widget", -1));
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(actualName.toUtf8().constData(), -1));
    return TCL_OK;
}

static int tcl_cgraph_standalone(ClientData clientData, Tcl_Interp* interp,
                                int objc, Tcl_Obj* const objv[])
{
    if (objc < 2 || objc > 6) {  // name, mode, title, script, geometry
        Tcl_WrongNumArgs(interp, 1, objv, "name ?mode? ?title? ?script? ?geometry?");
        return TCL_ERROR;
    }
    
    QString name = QString::fromUtf8(Tcl_GetString(objv[1]));

    auto& manager = EssScriptableManager::getInstance();
    if (!manager.isWidgetNameAvailable(name)) {
        QString errorMsg = QString("widget '%1' already exists").arg(name);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(errorMsg.toUtf8().constData(), -1));
        return TCL_ERROR;
    }
    
    // Parse mode (default to normal)
    EssStandaloneWindow::WindowBehavior behavior = EssStandaloneWindow::Normal;
    if (objc >= 3) {
        QString mode = QString::fromUtf8(Tcl_GetString(objv[2])).toLower();
        if (mode == "tool") behavior = EssStandaloneWindow::UtilityWindow;
        else if (mode == "ontop") behavior = EssStandaloneWindow::AlwaysOnTop;
        else if (mode == "visible") behavior = EssStandaloneWindow::StayVisible;
    }
    
    int behaviorInt = static_cast<int>(behavior);
    
    // Optional title (defaults to name)
    QString title = name;
    if (objc >= 4) {
        title = QString::fromUtf8(Tcl_GetString(objv[3]));
    }
    
    // Optional script
    QString script;
    if (objc >= 5) {
        script = QString::fromUtf8(Tcl_GetString(objv[4]));
    }
    
    // Optional geometry string (X11 format)
    QString geometry = "600x400";  // default
    if (objc >= 6) {
        geometry = QString::fromUtf8(Tcl_GetString(objv[5]));
    }
    
    // Get workspace manager via main window
    auto* app = EssApplication::instance();
    if (app && app->mainWindow() && app->mainWindow()->workspace()) {
        QMetaObject::invokeMethod(app->mainWindow()->workspace(), 
                                "createStandaloneCGraphWidget",
                                Qt::QueuedConnection,
                                Q_ARG(QString, name),
                                Q_ARG(QString, title), 
                                Q_ARG(int, behaviorInt),
                                Q_ARG(QString, script),
                                Q_ARG(QString, geometry));
    } else {
        Tcl_SetResult(interp, "Application or workspace manager not available", TCL_STATIC);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

static int tcl_list_graphics_widgets(ClientData clientData, Tcl_Interp* interp,
                                      int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QStringList widgets = manager.getAllGraphicsWidgets();
    
    Tcl_Obj* listObj = Tcl_NewListObj(0, nullptr);
    for (const QString& name : widgets) {
        Tcl_ListObjAppendElement(interp, listObj, 
            Tcl_NewStringObj(name.toUtf8().constData(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

static int tcl_send_to_graphics_widget(ClientData clientData, Tcl_Interp* interp,
                                        int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "widget_name script");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString widgetName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString script = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    bool success = manager.sendScriptToGraphicsWidget(widgetName, script);
    
    if (!success) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Graphics widget not found or script failed", -1));
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

static int tcl_broadcast_to_graphics_widgets(ClientData clientData, Tcl_Interp* interp,
                                             int objc, Tcl_Obj* const objv[])
{
    Q_UNUSED(clientData)
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script");
        return TCL_ERROR;
    }
    
    auto& manager = EssScriptableManager::getInstance();
    QString script = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    int result = manager.broadcastToGraphicsWidgets(script);
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(manager.getAllGraphicsWidgets().size()));
    return result;
}



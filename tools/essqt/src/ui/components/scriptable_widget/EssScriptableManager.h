// EssScriptableManager.h
#ifndef ESSSCRIPTABLEMANAGER_H
#define ESSSCRIPTABLEMANAGER_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <tcl.h>

class EssScriptableWidget;

/**
 * @brief Manager for scriptable widgets - handles registration and command routing
 * 
 * Similar to QtCGManager but for the new scriptable widget system.
 * Provides centralized management, command routing, and group operations.
 */
class EssScriptableManager : public QObject
{
    Q_OBJECT

public:
    static EssScriptableManager& getInstance();
    
    // Widget registration
    QString registerWidget(const QString& name, EssScriptableWidget* widget);
    bool unregisterWidget(const QString& name);
    
    // Widget access
    EssScriptableWidget* getWidget(const QString& name) const;
    QList<QString> getAllWidgetNames() const;
    QList<EssScriptableWidget*> getAllWidgets() const;
    
    // Command routing
    int send(const QString& widgetName, const QString& command);
    int broadcast(const QString& command);
    int sendToGroup(const QString& groupTag, const QString& command);
    int sendToType(const QString& widgetType, const QString& command);
    
    // Group management
    void addToGroup(const QString& widgetName, const QString& groupTag);
    void removeFromGroup(const QString& widgetName, const QString& groupTag);
    QStringList getGroupMembers(const QString& groupTag) const;
    QStringList getAllGroups() const;
    
    // Widget type management
    QStringList getWidgetsByType(const QString& widgetType) const;
    QStringList getAllTypes() const;
    
    // Shared data (accessible to all widgets)
    void setSharedData(const QString& key, const QVariant& value);
    QVariant getSharedData(const QString& key) const;
    void clearSharedData();
    
    // Development helpers
    void setDevelopmentLayout(const QString& widgetName, int layoutMode);
    void broadcastDevelopmentMode(bool enabled);
    
    // Script management
    void setWidgetScript(const QString& widgetName, const QString& script);
    QString getWidgetScript(const QString& widgetName) const;
    void reloadAllScripts();

	// Graphics widget specific commands
	QString createGraphicsWidget(const QString& name = QString());
	bool sendScriptToGraphicsWidget(const QString& widgetName, const QString& script);
	int broadcastToGraphicsWidgets(const QString& script);
	QStringList getAllGraphicsWidgets() const;
	

	// Register Tcl commands with interpreter
	void registerTclCommands(Tcl_Interp* interp);
    void unregisterTclCommands(Tcl_Interp* interp);
    
signals:
    void widgetRegistered(const QString& name, EssScriptableWidget* widget);
    void widgetUnregistered(const QString& name);
    void commandSent(const QString& widgetName, const QString& command, int result);
    void groupModified(const QString& groupTag);
    void sharedDataChanged(const QString& key, const QVariant& value);
    void graphicsWidgetCreationRequested(const QString& name); 
    
private:
    EssScriptableManager() = default;
    ~EssScriptableManager() = default;
    EssScriptableManager(const EssScriptableManager&) = delete;
    EssScriptableManager& operator=(const EssScriptableManager&) = delete;
    
    QString generateUniqueName(const QString& prefix = "widget");
    void connectWidgetSignals(EssScriptableWidget* widget);
    void disconnectWidgetSignals(EssScriptableWidget* widget);

 // Static Tcl command implementations
    static int tcl_list_widgets(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_send_widget(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_broadcast_widgets(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_send_to_type(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_add_to_group(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_remove_from_group(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_send_to_group(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_list_groups(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_list_group_members(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_list_types(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_get_widget_type(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_exists(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_dev_mode(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_dev_layout(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_reload_scripts(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_shared_data(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_get_shared_data(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_clear_shared_data(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
          
private slots:
    void onWidgetDestroyed(QObject* widget);
    void onWidgetScriptExecuted(int result, const QString& output);

private:
    QMap<QString, EssScriptableWidget*> m_widgets;
    QMap<QString, QStringList> m_groups;  // group -> list of widget names
    QMap<QString, QVariant> m_sharedData;
    
    int m_nameCounter = 0;
};

#endif // ESSSCRIPTABLEMANAGER_H
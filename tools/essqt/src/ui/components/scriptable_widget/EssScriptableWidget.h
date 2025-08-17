// EssScriptableWidget.h
#ifndef ESSSCRIPTABLEWIDGET_H
#define ESSSCRIPTABLEWIDGET_H

#include <QWidget>
#include <QString>
#include <QVariant>
#include <QMap>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QDialog>
#include <QTimer>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <tcl.h>
#include "core/EssEventProcessor.h"
#include "EssScriptPrototypeStore.h" 
#include "EssEvent.h"

class EssCodeEditor;
class EssWidgetTerminal;
class EssWidgetConsole;

/**
 * @brief Base scriptable widget - handles Tcl interpreter and event binding
 * 
 * Focused responsibility: Tcl scripting, data binding, development tools
 * Does NOT handle graphics - that's for derived classes
 */
class EssScriptableWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssScriptableWidget(const QString& name = QString(), QWidget* parent = nullptr);
    virtual ~EssScriptableWidget();

    virtual QString getWidgetTypeName() const = 0;
    QString widgetTypeName() const { return getWidgetTypeName(); }
        
    // Core identity and scripting
    QString name() const { return m_name; }
    Tcl_Interp* interpreter() { return m_interp; }
    Tcl_Interp* mainInterpreter() const { return m_mainInterp; }
    void mainInterp(Tcl_Interp *interp) { m_mainInterp = interp; }
    virtual int eval(const QString& command);
    QString result() const;
    
    // Script management
    void setSetupScript(const QString& script);
    QString getSetupScript() const { return m_setupScript; }
    void executeSetupScript();
    
    // Event binding interface
    void bindDatapoint(const QString& dpointName, const QString& script);
    void bindEvent(const QString& eventPattern, const QString& script);
    
    // Shared data access
    void setMainInterpreter(Tcl_Interp* mainInterp) { m_mainInterp = mainInterp; }
    
	enum DevLayoutMode {
		DevBottomPanel,      // Script editor below main widget (tabbed)
		DevTabbed,           // Script editor in tabs with main widget
    	DevThreePanel,       // Main | Script | Terminal+Console (all visible)
	};
    
    void setDevelopmentMode(bool enabled);
    bool isDevelopmentMode() const { return m_developmentMode; }
    void setDevelopmentLayout(DevLayoutMode mode);
    DevLayoutMode developmentLayout() const { return m_devLayoutMode; }
  	void cleanupDevelopmentLayout();
    void showScriptEditor(bool show = true);
    void showLocalConsole(bool show = true);
    void triggerQuickSave();
    
protected:
    // Pure virtual interface for subclasses
    virtual void registerCustomCommands() = 0;
    virtual QWidget* createMainWidget() = 0;
    void initializeWidget();
    
    // Optional overrides
    bool eventFilter(QObject* obj, QEvent* event) override;
    virtual void onSetupComplete() {}
    virtual void onDatapointReceived(const QString& name, const QVariant& value, qint64 timestamp) {}
    virtual void onEventReceived(const EssEvent& event);
     
    // Utility methods for derived classes
    void localLog(const QString& message);
    QString substituteDatapointData(const QString& script, const QString& name,
                                   const QVariant& value, qint64 timestamp) const;

signals:
    void initialized();
    void scriptExecuted(int result, const QString& output);
    void datapointBound(const QString& pattern, const QString& script);
    void statusMessage(const QString& message, int timeout = 0);

private slots:
    void onDatapointReceived_internal(const QString& name, const QVariant& value, qint64 timestamp);
    void onDevelopmentModeToggled(bool enabled);
    void onLayoutModeChanged();
    void onTestScript();
    void onResetScript();
    void onGenerateCode();
    
    void onSavePrototypeRequested();
    void onQuickSaveRequested();
    void onLoadPrototypeRequested(); 
    void onMarkProductionRequested();
    void onResetInterpreterRequested();
    void onTestFromScratchRequested();
    void onGenerateEmbeddableRequested();
  
protected:
      virtual void applyDevelopmentLayout();
  
private:
    void initializeInterpreter();
    void registerCoreCommands();
    void initializeCorePackages();
    void setupDevelopmentUI();
    void connectToDataProcessor();
    void initializeMainInterpreterReference();
	
    
    bool matchesEventPattern(const QString& pattern, const EssEvent& event) const;
    QString substituteEventData(const QString& script, const EssEvent& event) const;
    EssEventProcessor* getEventProcessor() const;
    
    // Development UI layout methods
    void setupBottomPanelLayout();
    void setupTabbedLayout();
    void setupThreePanelLayout();
    void createScriptEditor();
    void createWidgetTerminal();
    void createWidgetConsole();
    void createDevelopmentToolbar();

    // Prototype development interface  
    void saveCurrentAsPrototype(const QString& name, const QString& description = QString());
    void loadPrototype(const QString& name);
    QStringList getAvailablePrototypes() const;
    void markCurrentAsProduction();
    QString generateEmbeddableScript() const;
    
    // Interpreter reset functionality
    void resetInterpreter();
    bool validateInterpreterState() const;
    
    // Development mode enhancements
    void setupPrototypeDevelopmentUI();
    void updatePrototypeUI();
    
    // Prototype state
    QString m_currentPrototypeName;
    ScriptPrototype m_currentPrototype;
    
    // New development actions
    QAction* m_savePrototypeAction = nullptr;
    QAction* m_loadPrototypeAction = nullptr;
    QAction* m_markProductionAction = nullptr;
    QAction* m_resetInterpreterAction = nullptr;
    QAction* m_testFromScratchAction = nullptr;
    QAction* m_quickSaveAction = nullptr;
    QComboBox* m_prototypeSelector = nullptr;
    QLabel* m_interpreterStatusLabel = nullptr;
    QLabel* m_prototypeStatusLabel = nullptr;

    
    // Core Tcl commands (minimal set)
    static int tcl_bind_datapoint(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_get_dg(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_put_dg(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);    
    static int tcl_local_log(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_test_datapoint(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);

    static int tcl_bind_event(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_list_event_types(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_list_event_subtypes(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_test_event(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    
    static int tcl_widget_set_script(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_get_script(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_resize(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_set_title(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_move(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_dev_mode(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_widget_dev_layout(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);    
    static int tcl_widget_close(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    
private:
    // Core state
    QString m_name;
    Tcl_Interp* m_interp;
    Tcl_Interp* m_mainInterp;
    QString m_setupScript;
    QString m_defaultSetupScript;
    bool m_initialized;
    
    // Event binding
    QMap<QString, QString> m_datapointBindings;
    QMap<QString, QString> m_eventBindings;
    
    bool matchesNumericPattern(const QString& pattern, const EssEvent& event) const;
     
    // Development mode components
    bool m_developmentMode;
    DevLayoutMode m_devLayoutMode;
    
    QWidget* m_functionalWidget;          // The actual component UI
    EssCodeEditor* m_scriptEditor;        // Script editor
    EssWidgetTerminal* m_widgetTerminal;  // Widget-specific terminal
    EssWidgetConsole* m_widgetConsole;    // Widget-specific console
    QToolBar* m_devToolbar;               // Development toolbar
    
    // Layout containers
    QVBoxLayout* m_mainLayout;        // Main container layout
    QSplitter* m_splitter;            // For side-by-side and bottom layouts
    QTabWidget* m_tabWidget;          // For tabbed layout
    QDialog* m_floatingEditor;        // For floating layout
    
    // Development actions
    QAction* m_toggleDevModeAction;
    QComboBox* m_layoutModeCombo;
    QAction* m_testScriptAction;
    QAction* m_resetScriptAction;
    QAction* m_generateCodeAction;
};

#endif // ESSSCRIPTABLEWIDGET_H
#pragma once
#include <QApplication>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QProgressBar>
#include <QFrame>
#include "DockManager.h"
#include "ConnectionManager.h"
#include "EssControlWidget.h"
#include "HostDiscoveryWidget.h"
#include "TerminalClient.h"
#include "CodeEditor.h"
#include "DservEventParser.h"
#include "TclInterp.h"
#include "TclConsoleWidget.h"
#include "DgTableWidget.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);

    QLabel* connectionStatusLabel;
    QLabel* systemStatusLabel;
    QLabel* observationStatusLabel;
    QProgressBar* progressBar;

    // Add these new method declarations
    void setupStatusBar();
    void showProgress(const QString& message, int value = 0);
    void hideProgress();

    // Tcl integration methods
    QString evaluateTcl(const QString& command);
    bool evaluateTclWithResult(const QString& command, QString& result);
    void setupTclCommands();

  // Data integration methods
    void loadDataFile(const QString& filename);
    void showDynGroupInViewer(DYN_GROUP* dg, const QString& name = QString());
      
    // Menu action slots
    void newProject();
    void openProject();
    void saveProject();
    void importData();
    void exportData();
    void showPreferences();
    void showConnectDialog();
    void showConnectionSettings();
    void showSystemSelector();
    void showUserGuide();
    void showKeyboardShortcuts();
    void showAbout();
		    
private slots:
    // Connection management
    void onHostConnected(const QString& host);
    void onHostDisconnected();
    void handleConnectionError(const QString& error);
    void handleEvent(const QString& msg);
    
    // ESS Control forwarding
    void onSubjectChanged(const QString& subject);
    void onSystemChanged(const QString& system);
    void onProtocolChanged(const QString& protocol);
    void onVariantChanged(const QString& variant);
    void onStartRequested();
    void onStopRequested(); 
    void onResetRequested();
    void reloadSystem();
    void reloadProtocol();
    void reloadVariant();
    void saveSettings();
    void resetSettings();
    
    // Terminal
    void sendCommand();
    void handleResponse(const QString &response);
    void handleError(const QString &error);
    
    // Dock management
    void onDockVisibilityChanged(DockType type, bool visible);

  // Tcl command slots
  void onTclCommandRequested(const QString& command);
						    
						    
public slots:
    void connectToHost(const QString& host);
    void disconnectFromHost();

protected:
  void closeEvent(QCloseEvent *event) override;
  
private:
    void setupComponents();
    void setupDocks();
    void setupMenus();
    void connectSignals();
    void clearWidgets();
    
    // Core components
    DockManager* dockManager;
    ConnectionManager* connectionManager;
TclInterp* tclInterpreter;  
    
    // Widgets (will be managed by DockManager)
    EssControlWidget* essControl;
    HostDiscoveryWidget* hostDiscovery;  // Add this
    CodeEditor* editor;
    QWidget* terminalWidget;
    QPlainTextEdit* terminalOutput;
  TclConsoleWidget *tclConsole;
  DgTableTabs *dgTables;
    QLineEdit* commandInput;
    QPushButton* sendButton;
    
    // Terminal client (separate from dock system)
    TerminalClient* client;
  
  // Tcl command callbacks (static methods that can be registered with Tcl)
  static int dg_view_func(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
  static int print_func(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
  static int qt_message_func(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
  static int load_data_func(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
  
  // Helper method to register Qt-specific Tcl commands
  void registerQtTclCommands();
  
    QString server = "127.0.0.1";
    int port = 2560;
};

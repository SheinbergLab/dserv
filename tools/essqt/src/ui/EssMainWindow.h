#pragma once

#include <QMainWindow>
#include <memory>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QLabel;
QT_END_NAMESPACE

// Components for our app
class EssTerminalWidget;
class EssOutputConsole;
class EssDatapointTableWidget;
class EssEventTableWidget;
class EssHostDiscoveryWidget;
class EssExperimentControlWidget;

class EssMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EssMainWindow(QWidget *parent = nullptr);
    ~EssMainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // File menu
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onPreferences();
    
    // Help menu
    void onAbout();
    void onAboutQt();
    
    // Status updates
    void updateStatus(const QString &message, int timeout = 0);

  void onShowTerminal();
  void onShowConsole();
  void onShowDatapointTable();
  void onShowEventTable();
  void onShowHostDiscovery();
  void onShowExperimentControl();
  
  void createDockWidgets();

      // Connection status slots
    void onConnected(const QString &host);
    void onDisconnected();
    void onConnectionError(const QString &error);

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    
    void readSettings();
    void writeSettings();
  void resetLayout();
  
    // Menus
    QMenu *m_fileMenu;
    QMenu *m_editMenu;
    QMenu *m_viewMenu;
    QMenu *m_toolsMenu;
    QMenu *m_helpMenu;
    
    // Status bar widgets
    QLabel *m_statusLabel;
    QLabel *m_connectionLabel;
    
// Terminal
EssTerminalWidget *m_terminal;
QDockWidget *m_terminalDock;
QAction *m_showTerminalAction;

// Console
EssOutputConsole *m_console;
QDockWidget *m_consoleDock;
QAction *m_showConsoleAction;

// Datapoint Table
EssDatapointTableWidget *m_datapointTable;
QDockWidget *m_datapointTableDock;
QAction *m_showDatapointTableAction;

// Event Table
EssEventTableWidget *m_eventTable;
QDockWidget *m_eventTableDock;
QAction *m_showEventTableAction;

// Host Discovery
EssHostDiscoveryWidget *m_hostDiscovery;
QDockWidget *m_hostDiscoveryDock;
QAction *m_showHostDiscoveryAction;

// Experiment Control  
  EssExperimentControlWidget *m_experimentControl;
  QDockWidget *m_experimentControlDock;
  QAction *m_showExperimentControlAction;
  
  void updateConnectionStatus(bool connected, const QString &host = QString());
};

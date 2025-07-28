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

  void onCommandExecuted(const QString &command);

  void onShowTerminal();
  void onShowConsole();
  void onShowDatapointTable();
void onShowEventTable();
  
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
    
    // UI Components
    EssTerminalWidget *m_terminal;
  EssOutputConsole *m_console;
  EssDatapointTableWidget *m_datapointTable;
 EssEventTableWidget *m_eventTable;

  // Dock widgets
  QDockWidget *m_terminalDock;
  QDockWidget *m_consoleDock;
  QDockWidget *m_datapointTableDock;
  QDockWidget *m_eventTableDock;

  QAction *m_showTerminalAction;
  QAction *m_showConsoleAction;
  QAction *m_showDatapointTableAction;
  QAction *m_showEventTableAction;
  
  void updateConnectionStatus(bool connected, const QString &host = QString());
};

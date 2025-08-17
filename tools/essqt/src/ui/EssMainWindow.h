#pragma once

#include <QMainWindow>

class QMenu;
class QAction;
class QLabel;
class EssWorkspaceManager;

class EssMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EssMainWindow(QWidget *parent = nullptr);
    ~EssMainWindow();

    EssWorkspaceManager* workspace() const { return m_workspace; }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // File menu
	void onOpenDatafile();
    void onCloseDatafile();
    void onSave();
    void onSaveAs();
    void onPreferences();
    
    // Help menu
    void onAbout();
    void onAboutQt();
    
    // Status updates
    void updateStatus(const QString &message, int timeout = 0);
    
    // Connection handling
    void onConnected(const QString &host);
    void onDisconnected();
    void onConnectionError(const QString &error);
    
private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void connectCommandInterface();
    void readSettings();
    void writeSettings();
    void updateConnectionStatus(bool connected, const QString &host);
    void updateMenuState();
    
    // Workspace manager handles all docks
    EssWorkspaceManager *m_workspace;
    
    // Menus
    QMenu *m_fileMenu;
    QMenu *m_editMenu;
    QMenu *m_viewMenu;
    QMenu *m_toolsMenu;
    QMenu *m_helpMenu;
    
    // Status bar
    QLabel *m_statusLabel;
    QLabel *m_connectionLabel;
};
#pragma once

#include <QObject>
#include <QMap>
#include <QList>

class QMainWindow;
class QDockWidget;
class QAction;
class EssTerminalWidget;
class EssOutputConsole;
class EssDatapointTableWidget;
class EssEventTableWidget;
class EssHostDiscoveryWidget;
class EssExperimentControlWidget;
class EssScriptEditorWidget;

class EssWorkspaceManager : public QObject
{
    Q_OBJECT

public:
    explicit EssWorkspaceManager(QMainWindow *mainWindow, QObject *parent = nullptr);
    ~EssWorkspaceManager();

    // Create and arrange all docks
    void setupWorkspace();
    
    // Access to widgets
    EssTerminalWidget* terminal() const { return m_terminal; }
    EssOutputConsole* console() const { return m_console; }
    EssDatapointTableWidget* datapointTable() const { return m_datapointTable; }
    EssEventTableWidget* eventTable() const { return m_eventTable; }
    EssHostDiscoveryWidget* hostDiscovery() const { return m_hostDiscovery; }
    EssExperimentControlWidget* experimentControl() const { return m_experimentControl; }
    EssScriptEditorWidget* scriptEditor() const { return m_scriptEditor; }
    
    // Dock visibility
    void setDockVisible(const QString &dockName, bool visible);
    bool isDockVisible(const QString &dockName) const;
    
    // Layout management
    void saveLayout();
    bool restoreLayout();  // Returns true if layout was restored
    void resetToDefaultLayout();

    // Get menu actions for View menu
    QList<QAction*> viewMenuActions() const;

signals:
    void statusMessage(const QString &message, int timeout);

private:
    void createLeftPanel();
    void createRightPanel();
    void createBottomPanel();
    void createScriptEditor();
    void connectSignals();

    QMainWindow *m_mainWindow;
    
    // Dock widgets
    QMap<QString, QDockWidget*> m_docks;
    
    // Widget instances
    EssTerminalWidget *m_terminal;
    EssOutputConsole *m_console;
    EssDatapointTableWidget *m_datapointTable;
    EssEventTableWidget *m_eventTable;
    EssHostDiscoveryWidget *m_hostDiscovery;
    EssExperimentControlWidget *m_experimentControl;
    EssScriptEditorWidget *m_scriptEditor;
};

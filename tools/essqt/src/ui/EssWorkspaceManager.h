#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QSize>
#include <functional>

QT_BEGIN_NAMESPACE
class QMainWindow;
class QDockWidget;
class QAction;
class QWidget;
QT_END_NAMESPACE

class EssTerminalWidget;
class EssOutputConsole;
class EssDatapointTableWidget;
class EssEventTableWidget;
class EssHostDiscoveryWidget;
class EssExperimentControlWidget;
class EssScriptEditorWidget;
class EssStimDgWidget;

class EssWorkspaceManager : public QObject
{
    Q_OBJECT

public:
    explicit EssWorkspaceManager(QMainWindow *mainWindow, QObject *parent = nullptr);
    ~EssWorkspaceManager();

    // Main setup
    void setupWorkspace();
    
    // Layout management
    void resetToDefaultLayout();
    void saveLayout();
    bool restoreLayout();
    
    // Dock visibility
    void setDockVisible(const QString &dockName, bool visible);
    bool isDockVisible(const QString &dockName) const;
    
    // Menu actions
    QList<QAction*> viewMenuActions() const;

signals:
    void statusMessage(const QString &message, int timeout = 0);

private:
    // Widget factory type
    using WidgetFactory = std::function<QWidget*()>;
    
    struct DockInfo {
        QString title;
        QString objectName;
        WidgetFactory factory;
        Qt::DockWidgetAreas allowedAreas = Qt::AllDockWidgetAreas;
        QSize minimumSize;
        QSize maximumSize;  // Add maximum size
        QSize preferredSize;
        // Additional constraints
        int minHeight = -1;  // -1 means no constraint
        int maxHeight = -1;
        int minWidth = -1;
        int maxWidth = -1;
    };
    
    // Setup methods
    void registerAllDocks();
    void createAllDocks();
    void applyDefaultLayout();
    void connectSignals();
    
    // Helper methods
    QDockWidget* createDock(const QString &id);
    QWidget* getWidget(const QString &id) const;
    
    // Meta-widget creators (return the container widget)
    QWidget* createControlPanel();
    
private:
    QMainWindow *m_mainWindow;
    
    // Dock registry - defines all available docks
    QMap<QString, DockInfo> m_dockRegistry;
    
    // Created docks and their widgets
    QMap<QString, QDockWidget*> m_docks;
    QMap<QString, QWidget*> m_widgets;
    
    // Direct widget pointers for convenience (optional)
    EssTerminalWidget *m_terminal;
    EssOutputConsole *m_console;
    EssDatapointTableWidget *m_datapointTable;
    EssEventTableWidget *m_eventTable;
    EssHostDiscoveryWidget *m_hostDiscovery;
    EssExperimentControlWidget *m_experimentControl;
    EssScriptEditorWidget *m_scriptEditor;
    EssStimDgWidget *m_stimDgViewer;
};
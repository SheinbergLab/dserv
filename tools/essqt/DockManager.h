#pragma once
#include <QObject>
#include <QMainWindow>
#include <QDockWidget>
#include <QWidget>
#include <QMenu>
#include <QString>
#include <QHash>

enum class DockArea {
    Left,
    Right, 
    Top,
    Bottom,
    Center  // For central widget
};

enum class DockType {
    // Control panels
    EssControl,
    HostDiscovery,        // New - for connection management
    SystemMonitor,
    DataVisualization,
    
    // Editors and viewers
    CodeEditor,
    LogViewer,
    DataViewer,

    // Communication
    Terminal,
    NetworkMonitor,
    TclConsole,
    
    // Analysis
    PerformanceAnalyzer,
    EventViewer
};

struct DockConfig {
    QString title;
    DockArea defaultArea;
    QDockWidget::DockWidgetFeatures features;
    bool defaultVisible;
    QString tabGroup;  // For grouping docks into tabs
    int priority;      // For ordering within tab groups
};

class DockManager : public QObject {
    Q_OBJECT
    
public:
    explicit DockManager(QMainWindow* mainWindow, QObject* parent = nullptr);
    
    // Register and create docks
    void registerDockType(DockType type, const DockConfig& config);
    QDockWidget* createDock(DockType type, QWidget* widget);
    
    // Dock management
    QDockWidget* getDock(DockType type) const;
    void showDock(DockType type);
    void hideDock(DockType type);
    void toggleDock(DockType type);
    
    // Layout management
    void setDefaultLayout();
    void saveLayout(const QString& name);
    void restoreLayout(const QString& name);
    QStringList getLayoutNames() const;
    
    // Tab group management
    void createTabGroup(const QString& groupName, const QList<DockType>& docks);
    void addToTabGroup(const QString& groupName, DockType dock);
    
    // Menu integration
    void setupViewMenu(QMenu* viewMenu);
    
signals:
    void dockVisibilityChanged(DockType type, bool visible);
    void layoutChanged();

private:
    Qt::DockWidgetArea mapDockArea(DockArea area) const;
    void applyTabGroups();
    void setupDockConnections(QDockWidget* dock, DockType type);
    
    QMainWindow* mainWindow;
    QHash<DockType, DockConfig> dockConfigs;
    QHash<DockType, QDockWidget*> docks;
    QHash<QString, QList<DockType>> tabGroups;
    QHash<QString, QByteArray> savedLayouts;
};

#include "DockManager.h"
#include <QSettings>
#include <QAction>
#include <QDebug>

DockManager::DockManager(QMainWindow* mainWindow, QObject* parent)
    : QObject(parent), mainWindow(mainWindow) {
    
    // Register default dock configurations
    registerDockType(DockType::EssControl, {
        "ESS Control",
        DockArea::Left,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
        true,
        "",  // No tab group - standalone  
        0    // Highest priority (top of left panel)
    });
    
    registerDockType(DockType::HostDiscovery, {
        "Connections",
        DockArea::Left,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
        true,
        "",  // No tab group - standalone
        1    // Below EssControl
    });
    
    registerDockType(DockType::CodeEditor, {
        "Code Editor", 
        DockArea::Right,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
        true,
        "",  // No tab group - standalone
        1    // Top on right side
    });

registerDockType(DockType::Terminal, {
    "Terminal",
    DockArea::Right, 
    QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
    true,
    "RightPanel",  // Tab group
    2             // Priority 2
});

registerDockType(DockType::TclConsole, {
    "Tcl Console",
    DockArea::Right,
    QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
    false,               // Hidden by default
    "RightPanel",        // Tab group
    3                    // Priority 3 (after Terminal)
});

 registerDockType(DockType::DataViewer, {
    "Data Viewer",
    DockArea::Right,     // Put it on the right panel
    QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
    true,                // Visible by default
    "",                  // No tab group - standalone
    0                    // High priority (top of right panel)
});
 
registerDockType(DockType::PerformanceAnalyzer, {
    "Performance",
    DockArea::Right,
    QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
    false,
    "RightPanel",        // Tab group 
    4                    // Priority 4 (after TclConsole)
});    
    
    registerDockType(DockType::LogViewer, {
        "System Log",
        DockArea::Bottom,
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable,
        false,  // Hidden by default
        "BottomPanel",
        1
    });
}

void DockManager::registerDockType(DockType type, const DockConfig& config) {
    dockConfigs[type] = config;
}

QDockWidget* DockManager::createDock(DockType type, QWidget* widget) {
    if (docks.contains(type)) {
        qWarning() << "Dock already exists for type:" << static_cast<int>(type);
        return docks[type];
    }
    
    if (!dockConfigs.contains(type)) {
        qWarning() << "No configuration found for dock type:" << static_cast<int>(type);
        return nullptr;
    }
    
    const DockConfig& config = dockConfigs[type];
    
    auto* dock = new QDockWidget(config.title, mainWindow);
    dock->setWidget(widget);
    dock->setFeatures(config.features);
    dock->setObjectName(QString("dock_%1").arg(static_cast<int>(type)));
    
    // Add to main window
    Qt::DockWidgetArea area = mapDockArea(config.defaultArea);
    mainWindow->addDockWidget(area, dock);
    
    // Set initial visibility
    dock->setVisible(config.defaultVisible);
    
    // Store reference
    docks[type] = dock;
    
    // Setup connections
    setupDockConnections(dock, type);
    
    // Add to tab group if specified
    if (!config.tabGroup.isEmpty()) {
        if (!tabGroups.contains(config.tabGroup)) {
            tabGroups[config.tabGroup] = QList<DockType>();
        }
        tabGroups[config.tabGroup].append(type);
    }
    
    return dock;
}

QDockWidget* DockManager::getDock(DockType type) const {
    return docks.value(type, nullptr);
}

void DockManager::showDock(DockType type) {
    if (auto* dock = getDock(type)) {
        dock->show();
        dock->raise();
    }
}

void DockManager::hideDock(DockType type) {
    if (auto* dock = getDock(type)) {
        dock->hide();
    }
}

void DockManager::toggleDock(DockType type) {
    if (auto* dock = getDock(type)) {
        if (dock->isVisible()) {
            hideDock(type);
        } else {
            showDock(type);
        }
    }
}


void DockManager::setDefaultLayout() {
    // Apply tab grouping (only for docks that have tab groups)
    applyTabGroups();
    
    // Set width constraints for left panel docks
    if (auto* essControlDock = getDock(DockType::EssControl)) {
        essControlDock->setMinimumWidth(280);
        essControlDock->setMaximumWidth(350);
        essControlDock->resize(280, essControlDock->height());
    }
    
    if (auto* hostDiscoveryDock = getDock(DockType::HostDiscovery)) {
        hostDiscoveryDock->setMinimumWidth(250);
        hostDiscoveryDock->setMaximumWidth(350);
        hostDiscoveryDock->resize(280, hostDiscoveryDock->height());
    }
    
    // Set height preferences for right panel docks
    if (auto* editorDock = getDock(DockType::CodeEditor)) {
        // Editor gets more space by default
        editorDock->setMinimumHeight(200);
    }
    
    if (auto* terminalDock = getDock(DockType::Terminal)) {
        // Terminal gets less space by default
        terminalDock->setMinimumHeight(150);
        terminalDock->setMaximumHeight(300);
    }
    
    // Set height preferences for bottom panel docks
    if (auto* logViewerDock = getDock(DockType::LogViewer)) {
        logViewerDock->setMinimumHeight(120);
        logViewerDock->setMaximumHeight(250);
    }
    
    if (auto* tclConsoleDock = getDock(DockType::TclConsole)) {
        tclConsoleDock->setMinimumHeight(150);
        tclConsoleDock->setMaximumHeight(300);
    }
}

void DockManager::applyTabGroups() {
    for (auto it = tabGroups.begin(); it != tabGroups.end(); ++it) {
        const QString& groupName = it.key();
        QList<DockType>& dockList = it.value();
        
        if (dockList.size() < 2) continue;
        
        // Sort by priority
        std::sort(dockList.begin(), dockList.end(), [this](DockType a, DockType b) {
            return dockConfigs[a].priority < dockConfigs[b].priority;
        });
        
        // Tabify all docks in the group
        for (int i = 1; i < dockList.size(); ++i) {
            QDockWidget* firstDock = getDock(dockList[0]);
            QDockWidget* currentDock = getDock(dockList[i]);
            
            if (firstDock && currentDock) {
                mainWindow->tabifyDockWidget(firstDock, currentDock);
            }
        }
    }
}

void DockManager::createTabGroup(const QString& groupName, const QList<DockType>& docks) {
    tabGroups[groupName] = docks;
}

void DockManager::addToTabGroup(const QString& groupName, DockType dock) {
    if (!tabGroups.contains(groupName)) {
        tabGroups[groupName] = QList<DockType>();
    }
    tabGroups[groupName].append(dock);
}

void DockManager::setupViewMenu(QMenu* viewMenu) {
    viewMenu->clear();
    
    // Add actions for each dock
    for (auto it = docks.begin(); it != docks.end(); ++it) {
        DockType type = it.key();
        QDockWidget* dock = it.value();
        
        QAction* action = dock->toggleViewAction();
        action->setText(dockConfigs[type].title);
        viewMenu->addAction(action);
    }
    
    viewMenu->addSeparator();
    
    // Add layout management actions
    QAction* defaultLayoutAction = viewMenu->addAction("Restore Default Layout");
    connect(defaultLayoutAction, &QAction::triggered, this, &DockManager::setDefaultLayout);
    
    QAction* saveLayoutAction = viewMenu->addAction("Save Current Layout...");
    // TODO: Implement save dialog
    
    QAction* manageLayoutsAction = viewMenu->addAction("Manage Layouts...");
    // TODO: Implement layout management dialog
}

void DockManager::saveLayout(const QString& name) {
    QSettings settings;
    QByteArray state = mainWindow->saveState();
    settings.setValue(QString("layouts/%1").arg(name), state);
    savedLayouts[name] = state;
}

void DockManager::restoreLayout(const QString& name) {
    QSettings settings;
    QByteArray state = settings.value(QString("layouts/%1").arg(name)).toByteArray();
    if (!state.isEmpty()) {
        mainWindow->restoreState(state);
        savedLayouts[name] = state;
    }
}

QStringList DockManager::getLayoutNames() const {
    QSettings settings;
    settings.beginGroup("layouts");
    QStringList names = settings.childKeys();
    settings.endGroup();
    return names;
}

Qt::DockWidgetArea DockManager::mapDockArea(DockArea area) const {
    switch (area) {
        case DockArea::Left: return Qt::LeftDockWidgetArea;
        case DockArea::Right: return Qt::RightDockWidgetArea;
        case DockArea::Top: return Qt::TopDockWidgetArea;
        case DockArea::Bottom: return Qt::BottomDockWidgetArea;
        case DockArea::Center: return Qt::NoDockWidgetArea;  // Central widget
    }
    return Qt::LeftDockWidgetArea;
}

void DockManager::setupDockConnections(QDockWidget* dock, DockType type) {
    connect(dock, &QDockWidget::visibilityChanged, this, [this, type](bool visible) {
        emit dockVisibilityChanged(type, visible);
    });
}

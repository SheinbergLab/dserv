#include "EssWorkspaceManager.h"
#include "core/EssApplication.h"
#include "core/EssConfig.h"
#include "terminal/EssTerminalWidget.h"
#include "console/EssOutputConsole.h"
#include "dpoint_table/EssDatapointTableWidget.h"
#include "event_table/EssEventTableWidget.h"
#include "host_discovery/EssHostDiscoveryWidget.h"
#include "experiment_control/EssExperimentControlWidget.h"
#include "script_editor/EssScriptEditorWidget.h"
#include "dg_viewer/EssStimDgWidget.h"

#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QAction>

EssWorkspaceManager::EssWorkspaceManager(QMainWindow *mainWindow, QObject *parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
    , m_terminal(nullptr)
    , m_console(nullptr)
    , m_datapointTable(nullptr)
    , m_eventTable(nullptr)
    , m_hostDiscovery(nullptr)
    , m_experimentControl(nullptr)
    , m_scriptEditor(nullptr)
    , m_stimDgViewer(nullptr)
{
}

EssWorkspaceManager::~EssWorkspaceManager() = default;

void EssWorkspaceManager::setupWorkspace()
{
    // Configure main window dock options
    m_mainWindow->setDockOptions(
        QMainWindow::AnimatedDocks | 
        QMainWindow::AllowNestedDocks | 
        QMainWindow::AllowTabbedDocks
    );
    
    // Register all available docks
    registerAllDocks();
    
    // Create dock widgets
    createAllDocks();
    
    // Connect signals
    connectSignals();
    
    // Apply layout
    if (!restoreLayout()) {
        applyDefaultLayout();
    }
    
    // Log initialization
    if (m_console) {
        // Register console with manager if needed
        // EssConsoleManager::instance()->registerConsole("main", m_console);
        m_console->logSystem("EssQt Workspace Initialized", "Workspace");
    }
}

void EssWorkspaceManager::registerAllDocks()
{
    // Control Panel - Meta widget combining host discovery and experiment control
    m_dockRegistry["ControlPanel"] = {
        tr("Control Panel"),
        "ControlPanelDock",
        [this]() { return createControlPanel(); },
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea,
        QSize(300, 400),  // minimum size
        QSize(300, 1000), // no maximum size
        QSize(300, 600)   // preferred size
    };
    
    // Terminal - with height constraints
    m_dockRegistry["Terminal"] = {
        tr("Terminal"),
        "TerminalDock",
        [this]() { 
            m_terminal = new EssTerminalWidget();
            return m_terminal;
        },
        Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea,
        QSize(400, 80),  // minimum size
        QSize(),          // no maximum size
        QSize(800, 150),  // preferred size
        70,              // minHeight
        400               // maxHeight - prevent terminal from taking too much space
    };
    
    // Console - similar constraints to terminal
    m_dockRegistry["Console"] = {
        tr("Output Console"),
        "ConsoleDock",
        [this]() { 
            m_console = new EssOutputConsole();
            return m_console;
        },
        Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea,
        QSize(400, 80),  // minimum size
        QSize(),          // no maximum size
        QSize(800, 150),  // preferred size
        70,              // minHeight
        400               // maxHeight
    };
    
    // Datapoint Table
    m_dockRegistry["DatapointTable"] = {
        tr("Datapoint Monitor"),
        "DatapointDock",
        [this]() { 
            m_datapointTable = new EssDatapointTableWidget();
            return m_datapointTable;
        },
        Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea,
        QSize(300, 200),  // minimum size
        QSize(),          // no maximum size
        QSize(400, 300),  // preferred size
        -1,               // no minHeight constraint
        -1,               // no maxHeight constraint
        250,              // minWidth
        -1                // no maxWidth
    };
    
    // Event Table
    m_dockRegistry["EventTable"] = {
        tr("Event Log"),
        "EventDock",
        [this]() { 
            m_eventTable = new EssEventTableWidget();
            return m_eventTable;
        },
        Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea,
        QSize(300, 200),  // minimum size
        QSize(),          // no maximum size
        QSize(400, 300),  // preferred size
        150,              // minHeight - ensure some rows visible
        -1,               // no maxHeight
        250,              // minWidth
        -1                // no maxWidth
    };
    
    // Script Editor
    m_dockRegistry["ScriptEditor"] = {
        tr("Script Editor"),
        "ScriptEditorDock",
        [this]() { 
            m_scriptEditor = new EssScriptEditorWidget();
            return m_scriptEditor;
        },
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea,
        QSize(300, 400),  // minimum size
        QSize(),          // no maximum size
        QSize(400, 600),  // preferred size
        300,              // minHeight - need space for editing
        -1,               // no maxHeight
        300,              // minWidth - need space for code
        -1                // no maxWidth
    };
    
    // Stimulus Data Viewer
    m_dockRegistry["StimDgViewer"] = {
        tr("Stimdg"),
        "StimDgDock",
        [this]() { 
            m_stimDgViewer = new EssStimDgWidget();
            return m_stimDgViewer;
        },
        Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea,
        QSize(300, 200),  // minimum size
        QSize(),          // no maximum size
        QSize(400, 300)   // preferred size
        // No individual constraints - uses QSize constraints only
    };
}

void EssWorkspaceManager::createAllDocks()
{
    for (auto it = m_dockRegistry.begin(); it != m_dockRegistry.end(); ++it) {
        createDock(it.key());
    }
}

QDockWidget* EssWorkspaceManager::createDock(const QString &id)
{
    if (!m_dockRegistry.contains(id)) {
        return nullptr;
    }
    
    const DockInfo &info = m_dockRegistry[id];
    
    // Create dock widget
    QDockWidget *dock = new QDockWidget(info.title, m_mainWindow);
    dock->setObjectName(info.objectName);
    dock->setAllowedAreas(info.allowedAreas);
    
    // Create widget using factory
    QWidget *widget = info.factory();
    dock->setWidget(widget);
    
    // Apply size constraints to the dock widget
    if (!info.minimumSize.isEmpty()) {
        dock->setMinimumSize(info.minimumSize);
    }
    if (!info.maximumSize.isEmpty()) {
        dock->setMaximumSize(info.maximumSize);
    }
    
    // Apply individual dimension constraints if specified
    if (info.minHeight >= 0) {
        dock->setMinimumHeight(info.minHeight);
    }
    if (info.maxHeight >= 0) {
        dock->setMaximumHeight(info.maxHeight);
    }
    if (info.minWidth >= 0) {
        dock->setMinimumWidth(info.minWidth);
    }
    if (info.maxWidth >= 0) {
        dock->setMaximumWidth(info.maxWidth);
    }
    
    // Store references
    m_docks[id] = dock;
    m_widgets[id] = widget;
    
    // Special handling for bottom docks
    if (id == "Terminal" || id == "Console") {
        // Allow the dock to be very small
        dock->setMinimumHeight(50);
        
        // Ensure the widget inside can also be small
        if (widget) {
            widget->setMinimumHeight(50);
        }
    }
    
    return dock;
}

QWidget* EssWorkspaceManager::createControlPanel()
{
    // Create container widget
    QWidget *container = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Create and add host discovery (compact)
    m_hostDiscovery = new EssHostDiscoveryWidget(container);
    m_hostDiscovery->setMaximumHeight(65);
    layout->addWidget(m_hostDiscovery);
    
    // Create and add experiment control (takes remaining space)
    m_experimentControl = new EssExperimentControlWidget(container);
    layout->addWidget(m_experimentControl, 1);
    
    return container;
}

void EssWorkspaceManager::applyDefaultLayout()
{
    // Clear current layout
    for (auto dock : m_docks) {
        dock->setVisible(false);
        m_mainWindow->removeDockWidget(dock);
    }
    
    m_mainWindow->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    m_mainWindow->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    
    // Left side: Control Panel and Script Editor (tabbed)
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ControlPanel"]);
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ScriptEditor"]);
    m_mainWindow->tabifyDockWidget(m_docks["ControlPanel"], m_docks["ScriptEditor"]);
    
    // Right side: Tables and viewer
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["DatapointTable"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["EventTable"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["StimDgViewer"]);
    
    // Split tables vertically
    m_mainWindow->splitDockWidget(m_docks["DatapointTable"], m_docks["EventTable"], Qt::Vertical);
    
    // Tab stimulus viewer with datapoint table
    m_mainWindow->tabifyDockWidget(m_docks["DatapointTable"], m_docks["StimDgViewer"]);
    
    // Bottom: Terminal and Console (tabbed)
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Terminal"]);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Console"]);
    m_mainWindow->tabifyDockWidget(m_docks["Terminal"], m_docks["Console"]);
    
    if (m_docks["Terminal"]) {
    	m_docks["Terminal"]->resize(m_docks["Terminal"]->width(), 150);
	}

    // Show all docks and set active tabs
    for (auto dock : m_docks) {
        dock->setVisible(true);
    }
    
    // Raise default tabs
    m_docks["ControlPanel"]->raise();
    m_docks["DatapointTable"]->raise();
    m_docks["Terminal"]->raise();
}

void EssWorkspaceManager::connectSignals()
{
    // Terminal signals
    if (m_terminal) {
        connect(m_terminal, &EssTerminalWidget::statusMessage,
                this, &EssWorkspaceManager::statusMessage);
    }
    
    // Script editor signals
    if (m_scriptEditor) {
        connect(m_scriptEditor, &EssScriptEditorWidget::statusMessage,
                this, &EssWorkspaceManager::statusMessage);
                
        connect(m_scriptEditor, &EssScriptEditorWidget::scriptModified,
                [this](EssScriptEditorWidget::ScriptType type, bool modified) {
                    if (modified) {
                        QString scriptName = m_scriptEditor->scriptTypeToString(type);
                        emit statusMessage(QString("%1 script modified").arg(scriptName), 2000);
                    }
                });
    }
    
    // Host discovery signals
    if (m_hostDiscovery) {
        connect(m_hostDiscovery, &EssHostDiscoveryWidget::hostSelected,
                [this](const QString &host) {
                    emit statusMessage(QString("Selected host: %1").arg(host), 3000);
                });
    }
    
    // Experiment control signals
    if (m_experimentControl) {
        connect(m_experimentControl, &EssExperimentControlWidget::experimentStarted,
                [this]() { emit statusMessage("Experiment started", 3000); });
        connect(m_experimentControl, &EssExperimentControlWidget::experimentStopped,
                [this]() { emit statusMessage("Experiment stopped", 3000); });
    }
    
    // Stimulus viewer signals
    if (m_stimDgViewer) {
        connect(m_stimDgViewer, &EssStimDgWidget::trialSelected,
                [this](int trial) {
                    emit statusMessage(QString("Selected trial %1").arg(trial), 2000);
                });
    }
}

void EssWorkspaceManager::resetToDefaultLayout()
{
    applyDefaultLayout();
}

QList<QAction*> EssWorkspaceManager::viewMenuActions() const
{
    QList<QAction*> actions;
    
    // Create toggle actions for each dock
    for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
        QDockWidget *dock = it.value();
        QAction *action = dock->toggleViewAction();
        action->setShortcut(QKeySequence()); // Clear any default shortcuts
        actions.append(action);
    }
    
    // Add separator and reset action
    QAction *separator = new QAction(m_mainWindow);
    separator->setSeparator(true);
    actions.append(separator);
    
    QAction *resetAction = new QAction(tr("&Reset Layout"), m_mainWindow);
    connect(resetAction, &QAction::triggered, this, &EssWorkspaceManager::resetToDefaultLayout);
    actions.append(resetAction);
    
    return actions;
}

void EssWorkspaceManager::saveLayout()
{
    EssConfig *config = EssApplication::instance()->config();
    config->setWindowState(m_mainWindow->saveState());
    config->sync();
}

bool EssWorkspaceManager::restoreLayout()
{
    EssConfig *config = EssApplication::instance()->config();
    QByteArray state = config->windowState();
    if (!state.isEmpty()) {
        return m_mainWindow->restoreState(state);
    }
    return false;
}

void EssWorkspaceManager::setDockVisible(const QString &dockName, bool visible)
{
    if (m_docks.contains(dockName)) {
        m_docks[dockName]->setVisible(visible);
    }
}

bool EssWorkspaceManager::isDockVisible(const QString &dockName) const
{
    if (m_docks.contains(dockName)) {
        return m_docks[dockName]->isVisible();
    }
    return false;
}

QWidget* EssWorkspaceManager::getWidget(const QString &id) const
{
    return m_widgets.value(id, nullptr);
}
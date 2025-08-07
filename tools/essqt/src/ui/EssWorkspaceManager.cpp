#include "EssWorkspaceManager.h"
#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "core/EssCommandInterface.h"
#include "core/EssConfig.h"
#include "terminal/EssTerminalWidget.h"
#include "console/EssOutputConsole.h"
#include "dpoint_table/EssDatapointTableWidget.h"
#include "event_table/EssEventTableWidget.h"
#include "host_discovery/EssHostDiscoveryWidget.h"
#include "experiment_control/EssExperimentControlWidget.h"
#include "script_editor/EssScriptEditorWidget.h"
#include "dg_viewer/EssStimDgWidget.h"
#include "visualization/EssEyeTouchVisualizerWidget.h"
#include "state_system/EssStateSystemWidget.h"
#include "cgraph/EssCGraphWidget.h"
#include "cgraph/EssStandaloneCGraph.h"

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
    , m_eyeTouchVisualizer(nullptr)
    , m_stateSystemWidget(nullptr) 
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
    
    // Create dock widgets directly - no registry
    createDocks();
    
    // Connect signals
    connectSignals();
    
    // Apply layout
    if (!restoreLayout()) {
        applyDefaultLayout();
    }
    
    // Log initialization
    if (m_console) {
        m_console->logSystem("EssQt Workspace Initialized", "Workspace");
    }
}

void EssWorkspaceManager::createDocks()
{
    // Create Control Panel dock
    QDockWidget *controlDock = new QDockWidget(tr("Control Panel"), m_mainWindow);
    controlDock->setObjectName("ControlPanelDock");
    controlDock->setWidget(createControlPanel());
    m_docks["ControlPanel"] = controlDock;
    
    // Create Terminal dock - restrict to bottom only
    QDockWidget *terminalDock = new QDockWidget(tr("Terminal"), m_mainWindow);
    terminalDock->setObjectName("TerminalDock");
    terminalDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_terminal = new EssTerminalWidget();
    terminalDock->setWidget(m_terminal);
    m_docks["Terminal"] = terminalDock;
    
    // Create Console dock - restrict to bottom only
    QDockWidget *consoleDock = new QDockWidget(tr("Output Console"), m_mainWindow);
    consoleDock->setObjectName("ConsoleDock");
    consoleDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_console = new EssOutputConsole();
    consoleDock->setWidget(m_console);
    m_docks["Console"] = consoleDock;
    
    // Create Eye/Touch Visualizer dock with special floating behavior
    QDockWidget *eyeTouchDock = new QDockWidget(tr("Eye/Touch Monitor"), m_mainWindow);
    eyeTouchDock->setObjectName("EyeTouchVisualizerDock");
    m_eyeTouchVisualizer = new EssEyeTouchVisualizerWidget();
    eyeTouchDock->setWidget(m_eyeTouchVisualizer);
    
    // Connect to handle floating state changes
    connect(eyeTouchDock, &QDockWidget::topLevelChanged, [this, eyeTouchDock](bool floating) {
        if (floating) {
            // When floating, remove constraints and set a good default size
            eyeTouchDock->setMinimumSize(400, 500);
            eyeTouchDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            
            // If currently too narrow from being docked, resize to a nice square
            if (eyeTouchDock->width() < 500) {
                // Set a nice default floating size
                // The widget will be square-ish accounting for controls
                eyeTouchDock->resize(600, 700);
            }
            
            // Ensure the widget can be freely resized
            m_eyeTouchVisualizer->setMinimumSize(400, 500);
            m_eyeTouchVisualizer->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        } else {
            // When docking, restore the narrow constraints
            eyeTouchDock->setMinimumWidth(300);
            eyeTouchDock->setMaximumWidth(350);
            eyeTouchDock->setMinimumHeight(300);
            eyeTouchDock->setMaximumHeight(QWIDGETSIZE_MAX);
            
            // Also update the widget constraints
            m_eyeTouchVisualizer->setMinimumWidth(300);
            m_eyeTouchVisualizer->setMaximumWidth(350);
            m_eyeTouchVisualizer->setMinimumHeight(300);
            m_eyeTouchVisualizer->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    });
    
    m_docks["EyeTouchVisualizer"] = eyeTouchDock;
    
    // Create Event Table dock with similar floating behavior
    QDockWidget *eventDock = new QDockWidget(tr("Event Log"), m_mainWindow);
    eventDock->setObjectName("EventDock");
    m_eventTable = new EssEventTableWidget();
    eventDock->setWidget(m_eventTable);
    
    // Also handle Event Table floating
    connect(eventDock, &QDockWidget::topLevelChanged, [this, eventDock](bool floating) {
        if (floating) {
            eventDock->setMinimumSize(400, 300);
            eventDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            
            if (eventDock->width() < 500) {
                eventDock->resize(600, 400);
            }
            
            m_eventTable->setMinimumSize(400, 300);
            m_eventTable->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        } else {
            eventDock->setMinimumWidth(300);
            eventDock->setMaximumWidth(350);
            eventDock->setMinimumHeight(200);
            eventDock->setMaximumHeight(QWIDGETSIZE_MAX);
            
            m_eventTable->setMinimumWidth(300);
            m_eventTable->setMaximumWidth(350);
            m_eventTable->setMinimumHeight(200);
            m_eventTable->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    });
    
    m_docks["EventTable"] = eventDock;
    
    // Create State Debug dock
    QDockWidget *stateSystemDock = new QDockWidget(tr("State System"), m_mainWindow);
    stateSystemDock->setObjectName("stateSystemDock");
    m_stateSystemWidget = new EssStateSystemWidget();
    stateSystemDock->setWidget(m_stateSystemWidget);

  connect(stateSystemDock, &QDockWidget::topLevelChanged, [this, stateSystemDock](bool floating) {
        if (floating) {
            stateSystemDock->setMinimumSize(600, 400);
            stateSystemDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            
            if (stateSystemDock->width() < 700) {
                stateSystemDock->resize(800, 600);
            }
            
            m_stateSystemWidget->setMinimumSize(600, 400);
            m_stateSystemWidget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        } else {
            // When docked, allow more flexibility than Event Table since it has more content
            stateSystemDock->setMinimumWidth(400);
            stateSystemDock->setMaximumWidth(600);
            stateSystemDock->setMinimumHeight(300);
            stateSystemDock->setMaximumHeight(QWIDGETSIZE_MAX);
            
            m_stateSystemWidget->setMinimumWidth(400);
            m_stateSystemWidget->setMaximumWidth(600);
            m_stateSystemWidget->setMinimumHeight(300);
            m_stateSystemWidget->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    });
    
    m_docks["stateSystem"] = stateSystemDock;

	// CGraph Widget
	QDockWidget *cgraphDock = new QDockWidget(tr("Graphs"), m_mainWindow);
	cgraphDock->setObjectName("CGraphDock");
	m_cgraphWidget = new EssCGraphWidget();
	
	// Set the command interface
	auto commandInterface = EssApplication::instance()->commandInterface();
	m_cgraphWidget->setCommandInterface(commandInterface);
	
	cgraphDock->setWidget(m_cgraphWidget);
	
	// Handle floating behavior similar to other docks
	connect(cgraphDock, &QDockWidget::topLevelChanged, [this, cgraphDock](bool floating) {
		if (floating) {
			cgraphDock->setMinimumSize(600, 400);
			cgraphDock->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
			
			if (cgraphDock->width() < 600) {
				cgraphDock->resize(800, 600);
			}
			
			m_cgraphWidget->setMinimumSize(600, 400);
			m_cgraphWidget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		} else {
			// When docked, use reasonable constraints
			cgraphDock->setMinimumWidth(400);
			cgraphDock->setMaximumWidth(800);
			cgraphDock->setMinimumHeight(300);
			cgraphDock->setMaximumHeight(QWIDGETSIZE_MAX);
			
			m_cgraphWidget->setMinimumWidth(400);
			m_cgraphWidget->setMaximumWidth(800);
			m_cgraphWidget->setMinimumHeight(300);
			m_cgraphWidget->setMaximumHeight(QWIDGETSIZE_MAX);
		}
	});
	
	m_docks["CGraph"] = cgraphDock;

        
    m_testStandaloneWidget = new EssStandaloneCGraph("stimview");
    
    // Add as a dock
    QDockWidget* standaloneDock = 
        new QDockWidget(tr("Standalone CGraph Test"), m_mainWindow);
    standaloneDock->setObjectName("StandaloneCGraphDock");
    standaloneDock->setWidget(m_testStandaloneWidget);
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, standaloneDock);
    m_docks["StandaloneCGraph"] = standaloneDock;
    
    // Connect signals
    connect(m_testStandaloneWidget, &EssStandaloneCGraph::initialized,
            [this]() {
        EssConsoleManager::instance()->logSuccess(
            "Standalone CGraph widget ready", "WorkspaceManager");
    });
    
    connect(m_testStandaloneWidget, &EssStandaloneCGraph::initializationFailed,
            [this](const QString& error) {
        EssConsoleManager::instance()->logError(
            QString("Standalone init failed: %1").arg(error), "WorkspaceManager");
    });
        
    // Create Script Editor dock
    QDockWidget *scriptDock = new QDockWidget(tr("Script Editor"), m_mainWindow);
    scriptDock->setObjectName("ScriptEditorDock");
    m_scriptEditor = new EssScriptEditorWidget();
    scriptDock->setWidget(m_scriptEditor);
    m_docks["ScriptEditor"] = scriptDock;
    
    // Create Stim Viewer dock
    QDockWidget *stimDock = new QDockWidget(tr("Stimdg"), m_mainWindow);
    stimDock->setObjectName("StimDgDock");
    m_stimDgViewer = new EssStimDgWidget();
    stimDock->setWidget(m_stimDgViewer);
    m_docks["StimDgViewer"] = stimDock;
    
    // Create Datapoint Table dock
    QDockWidget *dpointDock = new QDockWidget(tr("Datapoint Monitor"), m_mainWindow);
    dpointDock->setObjectName("DatapointDock");
    m_datapointTable = new EssDatapointTableWidget();
    dpointDock->setWidget(m_datapointTable);
    m_docks["DatapointTable"] = dpointDock;
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

void EssWorkspaceManager::initializeStandaloneWidgets()
{
    auto cmdInterface = EssApplication::instance()->commandInterface();
    
    if (m_testStandaloneWidget && !m_testStandaloneWidget->isInitialized()) {
        m_testStandaloneWidget->initialize(cmdInterface);
    }
    
    // Initialize other standalone widgets here...
}

void EssWorkspaceManager::applyDefaultLayout()
{
    // Clear current layout and hide all docks
    for (auto dock : m_docks) {
        dock->setVisible(false);
        dock->setFloating(false);
        m_mainWindow->removeDockWidget(dock);
    }
    
    // Set up corners
    m_mainWindow->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    m_mainWindow->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    m_mainWindow->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    m_mainWindow->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    
    // Build the layout structure
    
    // 1. Bottom area first - Terminal and Console tabbed
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Terminal"]);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Console"]);
    m_mainWindow->tabifyDockWidget(m_docks["Terminal"], m_docks["Console"]);
    
    // 2. Left area - Control Panel
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ControlPanel"]);
    
    // 3. Center-left - Eye/Touch above Event
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["EyeTouchVisualizer"]);
    m_mainWindow->splitDockWidget(m_docks["ControlPanel"], m_docks["EyeTouchVisualizer"], Qt::Horizontal);
    
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["EventTable"]);
    m_mainWindow->splitDockWidget(m_docks["EyeTouchVisualizer"], m_docks["EventTable"], Qt::Vertical);
    
    // 4. Right area - Script Editor, Stim Viewer, and Datapoint Monitor (tabbed)
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["ScriptEditor"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["stateSystem"]); 
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["StimDgViewer"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["DatapointTable"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["StandaloneCGraph"]);

 
    // Tab them together
    m_mainWindow->tabifyDockWidget(m_docks["ScriptEditor"], m_docks["stateSystem"]);
    m_mainWindow->tabifyDockWidget(m_docks["stateSystem"], m_docks["StimDgViewer"]);
    m_mainWindow->tabifyDockWidget(m_docks["StimDgViewer"], m_docks["DatapointTable"]);
    m_mainWindow->tabifyDockWidget(m_docks["DatapointTable"], m_docks["CGraph"]);
    m_mainWindow->tabifyDockWidget(m_docks["CGraph"], m_docks["StandaloneCGraph"]);

   
    // Set constraints for docked state only
    
    // Control Panel: prefer ~300px but flexible
    m_docks["ControlPanel"]->setMinimumWidth(250);
    m_docks["ControlPanel"]->setMaximumWidth(400);
    
    // Eye/Touch and Event: constrain width only when docked
    // The topLevelChanged handlers will manage these dynamically
    if (!m_docks["EyeTouchVisualizer"]->isFloating()) {
        m_docks["EyeTouchVisualizer"]->setMinimumWidth(300);
        m_docks["EyeTouchVisualizer"]->setMaximumWidth(350);
        m_eyeTouchVisualizer->setMinimumWidth(300);
        m_eyeTouchVisualizer->setMaximumWidth(350);
    }
    
    if (!m_docks["EventTable"]->isFloating()) {
        m_docks["EventTable"]->setMinimumWidth(300);
        m_docks["EventTable"]->setMaximumWidth(350);
        m_eventTable->setMinimumWidth(300);
        m_eventTable->setMaximumWidth(350);
    }
    
    // Terminal/Console: limit height to prevent taking too much space
    m_docks["Terminal"]->setMinimumHeight(80);
    m_docks["Terminal"]->setMaximumHeight(200);
    m_docks["Console"]->setMinimumHeight(80);
    m_docks["Console"]->setMaximumHeight(200);
    
    // Restrict Terminal/Console to bottom only
    m_docks["Terminal"]->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_docks["Console"]->setAllowedAreas(Qt::BottomDockWidgetArea);
    
    // Set reasonable initial sizes as hints (Qt will adjust for screen size)
    QSize mainSize = m_mainWindow->size();
    if (mainSize.width() > 100 && mainSize.height() > 100) {
        // Use proportions from your layout
        int controlWidth = mainSize.width() * 0.24;  // ~24% for control panel
        int centerWidth = mainSize.width() * 0.24;   // ~24% for eye/event column
        int terminalHeight = mainSize.height() * 0.18; // ~18% for terminal
        
        m_docks["ControlPanel"]->resize(controlWidth, m_docks["ControlPanel"]->height());
        m_docks["EyeTouchVisualizer"]->resize(centerWidth, m_docks["EyeTouchVisualizer"]->height());
        m_docks["EventTable"]->resize(centerWidth, m_docks["EventTable"]->height());
        m_docks["Terminal"]->resize(m_docks["Terminal"]->width(), terminalHeight);
    }
    
    // Show all docks
    for (auto dock : m_docks) {
        dock->setVisible(true);
    }
    
    // Set active tabs
    m_docks["ScriptEditor"]->raise();
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
        bool restored = m_mainWindow->restoreState(state);
        
        // After restoring, ensure floating docks are raised
        if (restored) {
            for (auto dock : m_docks) {
                if (dock->isFloating() && dock->isVisible()) {
                    // Raise floating docks to front
                    dock->raise();
                    dock->activateWindow();
                }
            }
        }
        
        return restored;
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
    if (m_docks.contains(id)) {
        return m_docks[id]->widget();
    }
    return nullptr;
}
// EssWorkspaceManager.cpp

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
#include "dg_viewer/EssDgViewerWidget.h"
#include "visualization/EssEyeTouchVisualizerWidget.h"
#include "state_system/EssStateSystemWidget.h"
#include "scriptable_widget/EssScriptableManager.h"
#include "cgraph/DraggableTabWidget.h"
#include "cgraph/EssGraphicsWidget.h"
#include "behavmon/EssBehavmonWidget.h"

#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QAction>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>

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
	, m_cgraphTabWidget(nullptr)
	, m_cgraphDock(nullptr) 
	, m_dgViewerWidget(nullptr)   
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

void EssWorkspaceManager::createCGraphWidget(const QString& name, const QString& title)
{
    // Create EssGraphicsWidget
    EssGraphicsWidget* graph = new EssGraphicsWidget(name);
    
    auto cmdInterface = EssApplication::instance()->commandInterface();
    graph->mainInterp(cmdInterface->tclInterp());
      
    // Enable development mode by default for new widgets
    graph->setDevelopmentMode(true);
    graph->setDevelopmentLayout(EssScriptableWidget::DevBottomPanel);
    
    // Store in map (now properly typed)
    m_cgraphs[name] = graph;
    
    // Register with scriptable manager
    EssScriptableManager::getInstance().registerWidget(name, graph);
    
    // Add to tab widget
    int index = m_cgraphTabWidget->addTab(graph, title.isEmpty() ? name : title);
    m_cgraphTabWidget->setCurrentIndex(index);
    
    // Show and raise
    m_cgraphDock->show();
    m_cgraphDock->raise();
    
    // Connect signals
    connect(graph, &QObject::destroyed, [this, name]() {
        m_cgraphs.remove(name);
        EssScriptableManager::getInstance().unregisterWidget(name);
    });
    
    connect(graph, &EssGraphicsWidget::returnToTabsRequested, [this, name]() {
        if (m_docks.contains(name)) {
            returnCGraphToTabs(m_docks[name], name);
        }
    });
    
    // Connect floating request to existing dock system
    connect(graph, &EssGraphicsWidget::floatingRequested, [this, name](bool floating) {
        handleGraphicsFloatingRequest(name, floating);
    });
    
    connect(graph, &EssScriptableWidget::statusMessage,
            this, &EssWorkspaceManager::statusMessage);
    
    updateCGraphMenu();
}

void EssWorkspaceManager::handleGraphicsFloatingRequest(const QString& name, bool floating)
{
    if (!m_cgraphs.contains(name)) {
        return;
    }
    
    EssGraphicsWidget* graph = m_cgraphs[name];
    
    if (floating) {
        // Find the tab index and detach it
        for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
            if (m_cgraphTabWidget->widget(i) == graph) {
                detachCGraphTab(i);
                break;
            }
        }
    } else {
        // Return from floating to tabs
        if (m_docks.contains(name)) {
            returnCGraphToTabs(m_docks[name], name);
        }
    }
}

void EssWorkspaceManager::onCGraphTabCloseRequested(int index)
{
    QWidget* widget = m_cgraphTabWidget->widget(index);
    
    // Only handle EssGraphicsWidget now
    auto essgraph = qobject_cast<EssGraphicsWidget*>(widget);
    if (!essgraph) {
        return; // Not a graphics widget
    }
    
    QString name = essgraph->name();
    
    // Ask for confirmation
    QMessageBox::StandardButton reply = QMessageBox::question(
        m_mainWindow, 
        tr("Close Graphics Widget"),
        tr("Close graphics widget '%1'?").arg(name),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        m_cgraphTabWidget->removeTab(index);
        m_cgraphs.remove(name);
        
        // Unregister from scriptable manager
        EssScriptableManager::getInstance().unregisterWidget(name);
        
        widget->deleteLater();
        updateCGraphMenu();
    }
}

void EssWorkspaceManager::detachCGraphTab(int index)
{
    QWidget* widget = m_cgraphTabWidget->widget(index);
    
    // Only handle EssGraphicsWidget now
    auto essgraph = qobject_cast<EssGraphicsWidget*>(widget);
    if (!essgraph) {
        return; // Not a graphics widget
    }
    
    QString name = essgraph->name();
    QString title = m_cgraphTabWidget->tabText(index);
    
    // Set floating mode BEFORE removing from tab widget
    essgraph->setFloatingMode(true);
    
    // Remove from tab widget
    m_cgraphTabWidget->removeTab(index);
    
    // Create floating dock
    QDockWidget* dock = new QDockWidget(title, m_mainWindow);
    dock->setObjectName(QString("%1Dock").arg(name));
    dock->setWidget(widget);
    dock->setFloating(true);
    
    // Add context menu for re-docking
    dock->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dock, &QDockWidget::customContextMenuRequested,
            [this, dock, name](const QPoint& pos) {
                QMenu menu;
                QAction* retabAction = menu.addAction(tr("Return to Tabs"));
                connect(retabAction, &QAction::triggered, [this, dock, name]() {
                    returnCGraphToTabs(dock, name);
                });
                menu.exec(dock->mapToGlobal(pos));
            });
    
    dock->installEventFilter(this);
    m_docks[name] = dock;
    
    // Add to main window and show
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    
    // Use Qt's signal to know when floating is complete
    connect(dock, &QDockWidget::topLevelChanged, [essgraph](bool floating) {
        if (floating && essgraph) {
            // Use Qt's proper deferred execution
            QMetaObject::invokeMethod(essgraph, "forceGraphicsResize", Qt::QueuedConnection);
        }
    });
    
    dock->show();
    
    updateCGraphMenu();
}

void EssWorkspaceManager::returnCGraphToTabs(QDockWidget* dock, const QString& name)
{
    if (!dock) return;
    
    QWidget* widget = dock->widget();
    if (!widget) return;
    
    // Only handle EssGraphicsWidget now
    auto essgraph = qobject_cast<EssGraphicsWidget*>(widget);
    if (essgraph) {
        essgraph->setFloatingMode(false);
        
        // Update the floating button state to unchecked using public accessor
        QAction* floatingAction = essgraph->getFloatingAction();
        if (floatingAction) {
            floatingAction->setChecked(false);
        }
    }
        
    // Remove from dock
    dock->setWidget(nullptr);
    
    // Add back to tabs
    QString title = dock->windowTitle();
    int index = m_cgraphTabWidget->addTab(widget, title);
    m_cgraphTabWidget->setCurrentIndex(index);
    
    // Clean up dock
    m_mainWindow->removeDockWidget(dock);
    m_docks.remove(name);
    dock->deleteLater();
    
    // Make sure tab container is visible
    m_cgraphDock->show();
    m_cgraphDock->raise();
    
    if (m_cgraphDock->isFloating()) {
        m_cgraphDock->setFloating(false);
    }
    
    QList<QDockWidget*> tabifiedDocks = m_mainWindow->tabifiedDockWidgets(m_cgraphDock);
    if (!tabifiedDocks.isEmpty()) {
        m_cgraphDock->raise();
    }
    
    updateCGraphMenu();
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
    
    m_docks["EyeTouchVisualizer"] = eyeTouchDock;
    
    // Create Event Table dock with similar floating behavior
    QDockWidget *eventDock = new QDockWidget(tr("Event Log"), m_mainWindow);
    eventDock->setObjectName("EventDock");
    m_eventTable = new EssEventTableWidget();
    eventDock->setWidget(m_eventTable);
    
    m_docks["EventTable"] = eventDock;
    
    // BehavMon dock
	QDockWidget *behavmonDock = new QDockWidget(tr("Performance Monitor"), m_mainWindow);
	behavmonDock->setObjectName("BehavmonDock");
	m_behavmonWidget = new EssBehavmonWidget();
	behavmonDock->setWidget(m_behavmonWidget);

	behavmonDock->setMinimumWidth(220);  // Reduced from default
	behavmonDock->setMinimumHeight(250); // Minimum functional height

	m_docks["BehavMon"] = behavmonDock;

    // Create State Debug dock
    QDockWidget *stateSystemDock = new QDockWidget(tr("State System"), m_mainWindow);
    stateSystemDock->setObjectName("stateSystemDock");
    m_stateSystemWidget = new EssStateSystemWidget();
    stateSystemDock->setWidget(m_stateSystemWidget);
  
    m_docks["stateSystem"] = stateSystemDock;

    // Create CGraph tabbed dock
    m_cgraphDock = new QDockWidget(tr("CGraph Windows"), m_mainWindow);
    m_cgraphDock->setObjectName("CGraphDock");
    
    // Create a container widget to ensure proper spacing and consistent dock title height
    QWidget* container = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);  // No margins on container
    layout->setSpacing(0);
    
    // Create draggable tab widget for CGraphs
    m_cgraphTabWidget = new DraggableTabWidget();
    
    // Add tab widget to container layout
    layout->addWidget(m_cgraphTabWidget);
    
    // Set the container as the dock widget (not the tab widget directly)
    m_cgraphDock->setWidget(container);
    
    // Connect signals for drag-out functionality
    connect(m_cgraphTabWidget, &DraggableTabWidget::tabDetached,
            this, &EssWorkspaceManager::onCGraphTabDetached);
    
    // Connect tab close signal
    connect(m_cgraphTabWidget, &QTabWidget::tabCloseRequested,
            this, &EssWorkspaceManager::onCGraphTabCloseRequested);
    
    m_docks["CGraphContainer"] = m_cgraphDock;
    
    // Enable floating behavior for the entire dock
    m_cgraphDock->setFeatures(QDockWidget::DockWidgetMovable | 
                              QDockWidget::DockWidgetFloatable | 
                              QDockWidget::DockWidgetClosable);
    
    // Handle when the entire dock is floated
    connect(m_cgraphDock, &QDockWidget::topLevelChanged, [this](bool floating) {
        if (floating) {
            // When the entire tab container is floated, give it a good size
            if (m_cgraphDock->width() < 600) {
                m_cgraphDock->resize(800, 600);
            }
        }
    });
          
    // Create Script Editor dock
    QDockWidget *scriptDock = new QDockWidget(tr("Script Editor"),
					      m_mainWindow);
    scriptDock->setObjectName("ScriptEditorDock");
    m_scriptEditor = new EssScriptEditorWidget();
    scriptDock->setWidget(m_scriptEditor);
    m_docks["ScriptEditor"] = scriptDock;
    
    // Create Stimdg table viewer dock
    QDockWidget *stimDock = new QDockWidget(tr("Stimdg"), m_mainWindow);
    stimDock->setObjectName("StimDgDock");
    m_stimDgViewer = new EssStimDgWidget();
    stimDock->setWidget(m_stimDgViewer);
    m_docks["StimDgViewer"] = stimDock;
    
    // DG Viewer dock
    QDockWidget *dgViewerDock = new QDockWidget("Dynamic Group Viewer", m_mainWindow);
	dgViewerDock->setObjectName("DgViewerDock");
    m_dgViewerWidget = new EssDgViewerWidget();
    dgViewerDock->setWidget(m_dgViewerWidget);
    m_docks["DgViewer"] = dgViewerDock;
  
    // Let our command interpreter connect to the dgViewer
    auto* cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface) {
        cmdInterface->setDgViewer(m_dgViewerWidget);
    }
    
    // Create Datapoint Table dock
    QDockWidget *dpointDock = new QDockWidget(tr("Datapoint Monitor"),
					      m_mainWindow);
    dpointDock->setObjectName("DatapointDock");
    m_datapointTable = new EssDatapointTableWidget();
    dpointDock->setWidget(m_datapointTable);
    m_docks["DatapointTable"] = dpointDock;
    
    m_cgraphMenu = new QMenu(tr("CGraph Windows"), m_mainWindow);
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
    // CRITICAL: Clean up and validate all docks before using them
    
    // 1. First, clean up any existing layout state
    clearCurrentLayout();
    
    // 2. Validate all docks exist before proceeding
    if (!validateAllDocks()) {
        // If any docks are missing, recreate them
        createDocks();
    }
    
    // 3. Remove all docks from main window safely
    for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
        QDockWidget* dock = it.value();
        if (dock && !dock->isHidden()) {
            dock->setVisible(false);
            dock->setFloating(false);
            m_mainWindow->removeDockWidget(dock);
        }
    }
    
    // Set up corners
    m_mainWindow->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    m_mainWindow->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    m_mainWindow->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    m_mainWindow->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    
    // Build the layout structure with null checks
    
    // 1. Bottom area first - Terminal and Console tabbed
    if (m_docks.contains("Terminal") && m_docks.contains("Console") && 
        m_docks["Terminal"] && m_docks["Console"]) {
        
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Terminal"]);
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Console"]);
        m_mainWindow->tabifyDockWidget(m_docks["Terminal"], m_docks["Console"]);
    }
    
    // 2. Left area - Control Panel
    if (m_docks.contains("ControlPanel") && m_docks["ControlPanel"]) {
        m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ControlPanel"]);
    }
    
    // 3. Center-left - Eye/Touch above Event above behavmon
    if (m_docks.contains("ControlPanel") && m_docks.contains("EyeTouchVisualizer") &&
        m_docks["ControlPanel"] && m_docks["EyeTouchVisualizer"]) {
        
        m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["EyeTouchVisualizer"]);
        m_mainWindow->splitDockWidget(m_docks["ControlPanel"], m_docks["EyeTouchVisualizer"], Qt::Horizontal);
    }
    
    if (m_docks.contains("EyeTouchVisualizer") && m_docks.contains("EventTable") &&
        m_docks["EyeTouchVisualizer"] && m_docks["EventTable"]) {
        
        m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["EventTable"]);
        m_mainWindow->splitDockWidget(m_docks["EyeTouchVisualizer"], m_docks["EventTable"], Qt::Vertical);
    }
    
    // FIXED: Check for BehavMon dock properly
    if (m_docks.contains("EventTable") && m_docks.contains("BehavMon") &&
        m_docks["EventTable"] && m_docks["BehavMon"]) {
        
        m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["BehavMon"]);
        m_mainWindow->splitDockWidget(m_docks["EventTable"], m_docks["BehavMon"], Qt::Vertical);
    }
    
    // 4. Right area - Script Editor, State System, etc. (all with null checks)
    QStringList rightDocks = {"ScriptEditor", "stateSystem", "StimDgViewer", "DatapointTable", "CGraphContainer", "DgViewer"};
    QDockWidget* previousDock = nullptr;
    
    for (const QString& dockName : rightDocks) {
        if (m_docks.contains(dockName) && m_docks[dockName]) {
            QDockWidget* dock = m_docks[dockName];
            m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
            
            if (previousDock) {
                m_mainWindow->tabifyDockWidget(previousDock, dock);
            }
            previousDock = dock;
        }
    }
    
    // Apply size constraints with null checks
    applySizeConstraints();
    
    // Show all valid docks
    for (auto dock : m_docks) {
        if (dock) {
            dock->setVisible(true);
        }
    }
    
    // Set active tabs safely
    if (m_docks.contains("ScriptEditor") && m_docks["ScriptEditor"]) {
        m_docks["ScriptEditor"]->raise();
    }
    if (m_docks.contains("Terminal") && m_docks["Terminal"]) {
        m_docks["Terminal"]->raise();
    }
}

// NEW: Add validation and cleanup methods
void EssWorkspaceManager::clearCurrentLayout()
{
    // Clear any pending layout operations
    QApplication::processEvents();
    
    // Remove all docks from main window
    for (auto dock : m_docks) {
        if (dock) {
            m_mainWindow->removeDockWidget(dock);
        }
    }
}

bool EssWorkspaceManager::validateAllDocks()
{
    QStringList requiredDocks = {
        "Terminal", "Console", "ControlPanel", "EyeTouchVisualizer", 
        "EventTable", "BehavMon", "ScriptEditor", "stateSystem", 
        "StimDgViewer", "DatapointTable", "CGraphContainer", "DgViewer"
    };
    
    for (const QString& dockName : requiredDocks) {
        if (!m_docks.contains(dockName) || !m_docks[dockName]) {
            qWarning() << "Missing dock:" << dockName;
            return false;
        }
        
        // Check if the dock's widget is valid
        QDockWidget* dock = m_docks[dockName];
        if (!dock->widget()) {
            qWarning() << "Dock widget missing for:" << dockName;
            return false;
        }
    }
    
    return true;
}

void EssWorkspaceManager::applySizeConstraints()
{
    // Apply size constraints with null checks
    if (m_docks.contains("Terminal") && m_docks["Terminal"] && m_terminal) {
        m_docks["Terminal"]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        
        m_terminal->setMinimumHeight(80);
        m_terminal->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }
    
    if (m_docks.contains("Console") && m_docks["Console"] && m_console) {
        m_docks["Console"]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        
        m_console->setMinimumHeight(80);
        m_console->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }
    
    // Apply other constraints...
    if (m_docks.contains("ControlPanel") && m_docks["ControlPanel"]) {
        m_docks["ControlPanel"]->setMinimumWidth(250);
        m_docks["ControlPanel"]->setMaximumWidth(310);
    }
    
    // Apply floating constraints for Eye/Touch and Event docks
    applyFloatingConstraints();
}

void EssWorkspaceManager::applyFloatingConstraints()
{
    // Eye/Touch constraints (only if not floating)
    if (m_docks.contains("EyeTouchVisualizer") && m_docks["EyeTouchVisualizer"] && 
        m_eyeTouchVisualizer && !m_docks["EyeTouchVisualizer"]->isFloating()) {
        
 //       m_docks["EyeTouchVisualizer"]->setMinimumWidth(300);
 //       m_eyeTouchVisualizer->setMinimumWidth(300);
    }
    
    // Event table constraints (only if not floating)
    if (m_docks.contains("EventTable") && m_docks["EventTable"] && 
        m_eventTable && !m_docks["EventTable"]->isFloating()) {
        
 //       m_docks["EventTable"]->setMinimumWidth(300);
 //       m_eventTable->setMinimumWidth(300);
    }
}

void EssWorkspaceManager::resetDockConstraints()
{
    // Reset all docks to reasonable defaults
    for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
        QDockWidget* dock = it.value();
        if (!dock) continue;
        
        QString dockName = it.key();
        
        // Reset dock constraints
        dock->setMinimumSize(QSize(0, 0));
        dock->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
        
        // Reset widget constraints  
        if (dock->widget()) {
            dock->widget()->setMinimumSize(QSize(0, 0));
            dock->widget()->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
        }
        
        // Apply sensible minimums only
        if (dockName == "BehavMon") {
//            dock->setMinimumWidth(200);
//            dock->setMinimumHeight(250);
        } else if (dockName == "EyeTouchVisualizer" || dockName == "EventTable") {
//            dock->setMinimumWidth(250);
//            dock->setMinimumHeight(200);
        } else if (dockName == "Terminal" || dockName == "Console") {
            dock->setMinimumHeight(80);
            dock->setMaximumHeight(300);  // Keep this one for bottom docks
        } else if (dockName == "ControlPanel") {
//            dock->setMinimumWidth(200);
//            dock->setMaximumWidth(300);   // Keep this one - it's a control panel
        }
    }
    
    qDebug() << "Dock constraints reset to defaults";
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

    // Graphics widget requests from command interface (legacy)
    auto cmdInterface = EssApplication::instance()->commandInterface();
    connect(cmdInterface, &EssCommandInterface::createCGraphRequested,
            this, &EssWorkspaceManager::createCGraphWidget);

    // Graphics widget requests from scriptable manager (new system)
    auto& scriptableManager = EssScriptableManager::getInstance();
	connect(&scriptableManager, &EssScriptableManager::graphicsWidgetCreationRequested,
        this, [this](const QString& name) {
            createCGraphWidget(name, name);
        });

            
	if (m_behavmonWidget) {
    	connect(m_behavmonWidget, &EssBehavmonWidget::statusMessage,
        	    this, &EssWorkspaceManager::statusMessage);
	}
            
    // Menu update signal
    connect(m_cgraphMenu, &QMenu::aboutToShow,
            this, &EssWorkspaceManager::updateCGraphMenu); 
}

void EssWorkspaceManager::resetToDefaultLayout()
{
    // Add debug logging
    qDebug() << "Resetting layout...";
    
    resetDockConstraints();
    
    // Ensure all widgets are valid before resetting
    if (!validateAllDocks()) {
        qWarning() << "Some docks are invalid - recreating all docks";
        createDocks();
    }
    
    applyDefaultLayout();
    
    qDebug() << "Layout reset complete";
}

bool EssWorkspaceManager::eventFilter(QObject* obj, QEvent* event)
{
    // Handle double-click on floating CGraph dock title bars
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (QDockWidget* dock = qobject_cast<QDockWidget*>(obj)) {
            // Check if this is a CGraph dock
            QString dockName = dock->objectName();
            if (dockName.endsWith("Dock")) {
                QString possibleName = dockName.left(dockName.length() - 4);
                if (m_cgraphs.contains(possibleName)) {
                    QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
                    
                    // Check if click is on title bar area (not on the content)
                    if (mouseEvent->position().y() < 30) {  // Approximate title bar height
                        // If it's floating, return to tabs
                        // If it's docked, also return to tabs (your desired behavior)
                        returnCGraphToTabs(dock, possibleName);
                        return true;  // Consume the event to prevent default dock behavior
                    }
                }
            }
        }
    }
    return QObject::eventFilter(obj, event);
}

void EssWorkspaceManager::onCGraphTabDetached(QWidget* widget, const QString& title, const QPoint& globalPos)
{
    // Only handle EssGraphicsWidget now
    auto essgraph = qobject_cast<EssGraphicsWidget*>(widget);
    if (!essgraph) {
        return; // Not a graphics widget
    }
    
    QString name = essgraph->name();
    
    // Set floating mode
    essgraph->setFloatingMode(true);
    
    // Create floating dock
    QDockWidget* dock = new QDockWidget(title, m_mainWindow);
    dock->setObjectName(QString("%1Dock").arg(name));
    dock->setWidget(widget);
    dock->setFloating(true);
    dock->setWindowFlags(Qt::Tool);
    
    // Position at mouse location
    dock->move(globalPos - QPoint(100, 30));
    dock->resize(600, 400);
    
    dock->setFeatures(QDockWidget::DockWidgetMovable | 
                      QDockWidget::DockWidgetFloatable | 
                      QDockWidget::DockWidgetClosable);
    
    // Add context menu
    dock->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dock, &QDockWidget::customContextMenuRequested,
            [this, dock, name](const QPoint& pos) {
                QMenu menu;
                QAction* retabAction = menu.addAction(tr("Return to Tabs"));
                connect(retabAction, &QAction::triggered, [this, dock, name]() {
                    returnCGraphToTabs(dock, name);
                });
                menu.exec(dock->mapToGlobal(pos));
            });
    
    dock->installEventFilter(this);
    m_docks[name] = dock;
    
    // Add to main window and show
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->show();
    dock->raise();
    dock->activateWindow();
    
    updateCGraphMenu();
}

void EssWorkspaceManager::onCGraphRemoved(const QString& name)
{
    // Remove from tabs if present
    for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
        if (auto graph = qobject_cast<EssGraphicsWidget*>(m_cgraphTabWidget->widget(i))) {
            if (graph->name() == name) {
                m_cgraphTabWidget->removeTab(i);
                break;
            }
        }
    }
    
    // Remove from docks if floating
    if (m_docks.contains(name)) {
        QDockWidget* dock = m_docks.take(name);
        if (dock && !dock->isHidden()) {
            dock->deleteLater();
        }
    }
    
    // Remove from tracking
    m_cgraphs.remove(name);
}

void EssWorkspaceManager::updateCGraphMenu()
{
    m_cgraphMenu->clear();
    
    // Get widget names from tabs (only EssGraphicsWidget now)
    QStringList allGraphNames;
    for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
        if (auto essgraph = qobject_cast<EssGraphicsWidget*>(m_cgraphTabWidget->widget(i))) {
            allGraphNames.append(essgraph->name());
        }
    }
    
    bool hasTabbed = false;
    bool hasFloating = false;
    
    // Only show the "no widgets" message if we truly have no widgets
    // But don't return early - continue to show the creation option
    if (allGraphNames.isEmpty()) {
        QAction* emptyAction = m_cgraphMenu->addAction(tr("(No graphics widgets)"));
        emptyAction->setEnabled(false);
        // Don't return here - continue to show creation options
    } else {
        // Section for tabbed graphs
        for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
            auto essgraph = qobject_cast<EssGraphicsWidget*>(m_cgraphTabWidget->widget(i));
            if (!essgraph) continue;
            
            QString name = essgraph->name();
            QAction* action = m_cgraphMenu->addAction(name);
            action->setCheckable(true);
            action->setChecked(m_cgraphTabWidget->currentWidget() == essgraph);
            
            // Connect to show and select the tab when clicked
            connect(action, &QAction::triggered, [this, i]() {
                m_cgraphTabWidget->setCurrentIndex(i);
                m_cgraphDock->show();
                m_cgraphDock->raise();
            });
            hasTabbed = true;
        }
        
        // Section for floating graphs
        for (const QString& name : allGraphNames) {
            if (m_docks.contains(name)) {
                if (!hasFloating && hasTabbed) {
                    m_cgraphMenu->addSeparator();
                }
                QDockWidget* dock = m_docks[name];
                QAction* toggleAction = dock->toggleViewAction();
                toggleAction->setText(name + tr(" (floating)"));
                m_cgraphMenu->addAction(toggleAction);
                hasFloating = true;
            }
        }
    }
    
    // Always show creation and management actions (whether we have widgets or not)
    m_cgraphMenu->addSeparator();
    
    // Single simplified creation action - ALWAYS available
    QAction* newGraphAction = m_cgraphMenu->addAction(tr("New Graphics Widget"));
    connect(newGraphAction, &QAction::triggered, [this]() {
        bool ok;
        QString name = QInputDialog::getText(m_mainWindow, 
            tr("New Graphics Widget"), tr("Widget name:"), 
            QLineEdit::Normal, QString("graphics_%1").arg(m_cgraphs.size() + 1), &ok);
        
        if (ok && !name.isEmpty()) {
            createCGraphWidget(name, name);
        }
    });
    
    // Development mode toggle - only show if we have tabbed widgets
    if (hasTabbed) {
        m_cgraphMenu->addSeparator();
        
        QAction* toggleDevAction = m_cgraphMenu->addAction(tr("Toggle Development Mode"));
        connect(toggleDevAction, &QAction::triggered, [this]() {
            QWidget* currentWidget = m_cgraphTabWidget->currentWidget();
            if (auto graph = qobject_cast<EssGraphicsWidget*>(currentWidget)) {
                bool devMode = graph->isDevelopmentMode();
                graph->setDevelopmentMode(!devMode);
                
                QString mode = devMode ? "disabled" : "enabled";
                emit statusMessage(QString("Development mode %1 for %2").arg(mode, graph->name()), 3000);
            }
        });
    }
    
    // Management actions for existing widgets - only show if we have widgets
    if (hasTabbed || hasFloating) {
        m_cgraphMenu->addSeparator();
        
        if (hasTabbed) {
            QAction* detachAllAction = m_cgraphMenu->addAction(tr("Detach All Tabs"));
            connect(detachAllAction, &QAction::triggered, [this]() {
                while (m_cgraphTabWidget->count() > 0) {
                    detachCGraphTab(0);
                }
            });
        }
        
        if (hasFloating) {
            QAction* dockAllAction = m_cgraphMenu->addAction(tr("Return All to Tabs"));
            connect(dockAllAction, &QAction::triggered, [this]() {
                QList<QString> floatingNames;
                for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
                    if (m_cgraphs.contains(it.key())) {
                        floatingNames.append(it.key());
                    }
                }
                for (const QString& name : floatingNames) {
                    returnCGraphToTabs(m_docks[name], name);
                }
            });
        }
        
        QAction* closeAllAction = m_cgraphMenu->addAction(tr("Close All Graphics Widgets"));
        connect(closeAllAction, &QAction::triggered, [this]() {
            if (QMessageBox::question(m_mainWindow, tr("Close All Graphics Widgets"), 
                    tr("Close all graphics widgets?"), 
                    QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                
                // Close all tabbed graphs
                while (m_cgraphTabWidget->count() > 0) {
                    QWidget* widget = m_cgraphTabWidget->widget(0);
                    
                    if (auto essgraph = qobject_cast<EssGraphicsWidget*>(widget)) {
                        QString name = essgraph->name();
                        m_cgraphTabWidget->removeTab(0);
                        m_cgraphs.remove(name);
                        EssScriptableManager::getInstance().unregisterWidget(name);
                        widget->deleteLater();
                    }
                }
                
                // Close all floating graphs
                QList<QString> floatingNames;
                for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
                    if (m_cgraphs.contains(it.key())) {
                        floatingNames.append(it.key());
                    }
                }
                for (const QString& name : floatingNames) {
                    QDockWidget* dock = m_docks.take(name);
                    m_mainWindow->removeDockWidget(dock);
                    EssScriptableManager::getInstance().unregisterWidget(name);
                    dock->deleteLater();
                    m_cgraphs.remove(name);
                }
                
                updateCGraphMenu();
            }
        });
    }
}

void EssWorkspaceManager::sendScriptToCurrentGraphicsWidget(const QString& script)
{
    QWidget* currentWidget = m_cgraphTabWidget->currentWidget();
    if (auto graph = qobject_cast<EssGraphicsWidget*>(currentWidget)) {
        int result = graph->eval(script);
        QString output = graph->result();
        
        if (result == TCL_OK) {
            emit statusMessage(QString("Script executed successfully"), 2000);
        } else {
            emit statusMessage(QString("Script error: %1").arg(output), 5000);
        }
    }
}

QList<QAction*> EssWorkspaceManager::viewMenuActions() const
{
    QList<QAction*> actions;
    
    // Create toggle actions for each dock (skip CGraph individual docks)
    for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
        // Skip individual CGraph docks - they'll be in the submenu
        if (m_cgraphs.contains(it.key())) {
            continue;
        }
        
        QDockWidget *dock = it.value();
        QAction *action = dock->toggleViewAction();
        actions.append(action);
    }
    
    QAction *separator1 = new QAction(m_mainWindow);
    separator1->setSeparator(true);
    actions.append(separator1);
    
    // Add the CGraph submenu
    actions.append(m_cgraphMenu->menuAction());
            
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

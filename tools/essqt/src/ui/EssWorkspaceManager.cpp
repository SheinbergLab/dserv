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
#include "cgraph/qtcgraph.hpp"
#include "cgraph/qtcgmanager.hpp"
#include "cgraph/DraggableTabWidget.h"

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
    // Create the graph widget
    QtCGraph* graph = new QtCGraph(name);
    m_cgraphs[name] = graph;
    
    // Add to tab widget
    int index = m_cgraphTabWidget->addTab(graph, title.isEmpty() ? name : title);
    m_cgraphTabWidget->setCurrentIndex(index);
    
    // Graph dock should already be visible, but make sure
    m_cgraphDock->show();
    m_cgraphDock->raise();
    
    // Connect graph signals
    connect(graph, &QObject::destroyed, [this, name]() {
        m_cgraphs.remove(name);
    });
    
    // Connect the return to tabs signal
    connect(graph, &QtCGraph::returnToTabsRequested, [this, name]() {
        // Find the dock containing this graph
        if (m_docks.contains(name)) {
            returnCGraphToTabs(m_docks[name], name);
        }
    });
    
    updateCGraphMenu();
}

void EssWorkspaceManager::onCGraphTabCloseRequested(int index)
{
    QWidget* widget = m_cgraphTabWidget->widget(index);
    if (QtCGraph* graph = qobject_cast<QtCGraph*>(widget)) {
        QString name = graph->name();
        
        // Ask for confirmation if graph has content
        QMessageBox::StandardButton reply = QMessageBox::question(
            m_mainWindow, 
            tr("Close CGraph"),
            tr("Close graph '%1'?").arg(name),
            QMessageBox::Yes | QMessageBox::No
        );
        
        if (reply == QMessageBox::Yes) {
            m_cgraphTabWidget->removeTab(index);
            m_cgraphs.remove(name);
            graph->deleteLater();
            
            updateCGraphMenu();
        }
    }
}

// Add method to detach a tab into a floating window:
void EssWorkspaceManager::detachCGraphTab(int index)
{
    QWidget* widget = m_cgraphTabWidget->widget(index);
    if (QtCGraph* graph = qobject_cast<QtCGraph*>(widget)) {
        QString name = graph->name();
        QString title = m_cgraphTabWidget->tabText(index);
        
        // Remove from tab widget
        m_cgraphTabWidget->removeTab(index);
        
        // Create a new dock for this graph
        QDockWidget* dock = new QDockWidget(title, m_mainWindow);
        dock->setObjectName(QString("%1Dock").arg(name));
        dock->setWidget(graph);
        dock->setFloating(true);
        
        // Set a good size for floating window
        dock->resize(600, 400);
        
        // Add custom context menu for re-docking
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
        
        // Install event filter for double-click handling
        dock->installEventFilter(this);
        
        // Track in our docks
        m_docks[name] = dock;
        
        // Add to main window and show
        m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->show();
        
        updateCGraphMenu();
    }
}
void EssWorkspaceManager::returnCGraphToTabs(QDockWidget* dock, const QString& name)
{
    if (!dock) return;
    
    QtCGraph* graph = qobject_cast<QtCGraph*>(dock->widget());
    if (!graph) return;
    
    graph->setFloatingMode(false);
        
    // Remove from dock
    dock->setWidget(nullptr);
    
    // Add back to tabs
    QString title = dock->windowTitle();
    int index = m_cgraphTabWidget->addTab(graph, title);
    m_cgraphTabWidget->setCurrentIndex(index);
    
    // Clean up dock
    m_mainWindow->removeDockWidget(dock);
    m_docks.remove(name);
    dock->deleteLater();
    
    // Make sure tab container is visible and raised
    m_cgraphDock->show();
    m_cgraphDock->raise();
    
    // If the CGraph dock itself is floating, dock it first
    if (m_cgraphDock->isFloating()) {
        m_cgraphDock->setFloating(false);
    }
    
    // Switch to the CGraph tab in the tabbed dock area
    // Find which tab contains the CGraph dock and activate it
    QList<QDockWidget*> tabifiedDocks = m_mainWindow->tabifiedDockWidgets(m_cgraphDock);
    if (!tabifiedDocks.isEmpty()) {
        m_cgraphDock->raise();  // This should make it the active tab
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
            // When docked, allow more flexibility 
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
 	m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["CGraphContainer"]);

    // Tab them together
    m_mainWindow->tabifyDockWidget(m_docks["ScriptEditor"], m_docks["stateSystem"]);
    m_mainWindow->tabifyDockWidget(m_docks["stateSystem"], m_docks["StimDgViewer"]);
    m_mainWindow->tabifyDockWidget(m_docks["StimDgViewer"], m_docks["DatapointTable"]);
	m_mainWindow->tabifyDockWidget(m_docks["DatapointTable"], m_docks["CGraphContainer"]);
   
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

    // Cgraph widget requests    
    auto cmdInterface = EssApplication::instance()->commandInterface();
    connect(cmdInterface, &EssCommandInterface::createCGraphRequested,
        this, &EssWorkspaceManager::createCGraphWidget);

    // Connect to manager's graph removal signal
    auto& cgManager = QtCGManager::getInstance();
    connect(&cgManager, &QtCGManager::graphUnregistered,
            this, &EssWorkspaceManager::onCGraphRemoved);

    connect(m_cgraphMenu, &QMenu::aboutToShow,
            this, &EssWorkspaceManager::updateCGraphMenu);
}

void EssWorkspaceManager::resetToDefaultLayout()
{
    applyDefaultLayout();
}

void EssWorkspaceManager::onCGraphTabDetached(QWidget* widget, const QString& title, const QPoint& globalPos)
{
    QtCGraph* graph = qobject_cast<QtCGraph*>(widget);
    if (!graph) return;
    
    QString name = graph->name();
    
    graph->setFloatingMode(true);
    
    // Create a new floating dock for this graph
    QDockWidget* dock = new QDockWidget(title, m_mainWindow);
    dock->setObjectName(QString("%1Dock").arg(name));
    dock->setWidget(graph);
    dock->setFloating(true);
    
    // Position at mouse location
    dock->move(globalPos - QPoint(100, 30)); // Offset so title bar is near mouse
    
    // Set a good size for floating window
    dock->resize(600, 400);
    
    // Enable all dock features
    dock->setFeatures(QDockWidget::DockWidgetMovable | 
                      QDockWidget::DockWidgetFloatable | 
                      QDockWidget::DockWidgetClosable);
    
    // Add custom context menu for re-docking
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
    
    // Also support double-click on title bar to return to tabs
    dock->installEventFilter(this);
    
    // Track in our docks
    m_docks[name] = dock;
    
    // Add to main window and show
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->show();
    dock->raise();
    dock->activateWindow();
    
    updateCGraphMenu();
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

void EssWorkspaceManager::onCGraphRemoved(const QString& name)
{
    // Remove from tabs if present
    for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
        if (QtCGraph* graph = qobject_cast<QtCGraph*>(m_cgraphTabWidget->widget(i))) {
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
    
    // Remove from our tracking
    m_cgraphs.remove(name);
}

void EssWorkspaceManager::updateCGraphMenu()
{
    m_cgraphMenu->clear();
    
    auto& cgManager = QtCGManager::getInstance();
    QStringList graphNames = cgManager.getAllGraphNames();
    
    if (graphNames.isEmpty()) {
        QAction* emptyAction = m_cgraphMenu->addAction(tr("(No graphs)"));
        emptyAction->setEnabled(false);
        return;
    }
    
    // Section for tabbed graphs
    bool hasTabbed = false;
    for (int i = 0; i < m_cgraphTabWidget->count(); ++i) {
        if (QtCGraph* graph = qobject_cast<QtCGraph*>(m_cgraphTabWidget->widget(i))) {
            QString name = graph->name();
            QAction* action = m_cgraphMenu->addAction(name);
            action->setCheckable(true);
            action->setChecked(m_cgraphTabWidget->currentWidget() == graph);
            
            // Connect to show and select the tab when clicked
            connect(action, &QAction::triggered, [this, i]() {
                m_cgraphTabWidget->setCurrentIndex(i);
                m_cgraphDock->show();
                m_cgraphDock->raise();
            });
            hasTabbed = true;
        }
    }
    
    // Section for floating graphs
    bool hasFloating = false;
    for (const QString& name : graphNames) {
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
    
    // Management actions
    m_cgraphMenu->addSeparator();
    
    QAction* newGraphAction = m_cgraphMenu->addAction(tr("New Graph..."));
    connect(newGraphAction, &QAction::triggered, [this]() {
        bool ok;
        QString name = QInputDialog::getText(m_mainWindow, 
            tr("New CGraph"), tr("Graph name:"), 
            QLineEdit::Normal, QString("graph_%1").arg(m_cgraphs.size() + 1), &ok);
        
        if (ok && !name.isEmpty()) {
            createCGraphWidget(name, name);
        }
    });
    
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
        
        QAction* closeAllAction = m_cgraphMenu->addAction(tr("Close All Graphs"));
        connect(closeAllAction, &QAction::triggered, [this]() {
            if (QMessageBox::question(m_mainWindow, tr("Close All Graphs"), 
                    tr("Close all CGraph windows?"), 
                    QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                // Close all tabbed graphs
                while (m_cgraphTabWidget->count() > 0) {
                    QtCGraph* graph = qobject_cast<QtCGraph*>(m_cgraphTabWidget->widget(0));
                    m_cgraphTabWidget->removeTab(0);
                    if (graph) {
                        m_cgraphs.remove(graph->name());
                        graph->deleteLater();
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
                    dock->deleteLater();
                    m_cgraphs.remove(name);
                }
                
                updateCGraphMenu();
            }
        });
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

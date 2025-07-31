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

#include <QMainWindow>
#include <QDockWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QAction>
#include <QMenu>

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
{
}

EssWorkspaceManager::~EssWorkspaceManager() = default;

void EssWorkspaceManager::setupWorkspace()
{
    // Set dock options
    m_mainWindow->setDockOptions(
        QMainWindow::AnimatedDocks | 
        QMainWindow::AllowNestedDocks | 
        QMainWindow::AllowTabbedDocks
    );
    
    // Create all panels
    createLeftPanel();
    createRightPanel();
    createBottomPanel();
    createScriptEditor();
    
    // Connect signals
    connectSignals();
    
    // Restore saved layout or use default
    if (!restoreLayout()) {
        resetToDefaultLayout();
    }
}

void EssWorkspaceManager::createLeftPanel()
{
    // Create a control panel dock that will hold host discovery and experiment control
    QDockWidget *controlDock = new QDockWidget(tr("Control Panel"), m_mainWindow);
    controlDock->setObjectName("ControlPanelDock");
    
    // Create a container widget with vertical layout
    QWidget *controlContainer = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(controlContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Add host discovery at the top (compact)
    m_hostDiscovery = new EssHostDiscoveryWidget(controlContainer);
    m_hostDiscovery->setMaximumHeight(65); // Keep it compact
    layout->addWidget(m_hostDiscovery);
    
    // Add experiment control below (takes remaining space)
    m_experimentControl = new EssExperimentControlWidget(controlContainer);
    layout->addWidget(m_experimentControl, 1); // Stretch factor 1
    
    controlDock->setWidget(controlContainer);
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, controlDock);
    m_docks["ControlPanel"] = controlDock;
}

void EssWorkspaceManager::createRightPanel()
{
    // Create datapoint table dock
    QDockWidget *datapointDock = new QDockWidget(tr("Datapoint Monitor"), m_mainWindow);
    datapointDock->setObjectName("DatapointDock");
    m_datapointTable = new EssDatapointTableWidget(datapointDock);
    datapointDock->setWidget(m_datapointTable);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, datapointDock);
    m_docks["DatapointTable"] = datapointDock;
    
    // Create event table dock
    QDockWidget *eventDock = new QDockWidget(tr("Event Log"), m_mainWindow);
    eventDock->setObjectName("EventDock");
    m_eventTable = new EssEventTableWidget(eventDock);
    eventDock->setWidget(m_eventTable);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, eventDock);
    m_docks["EventTable"] = eventDock;
    
    // Split them horizontally
    m_mainWindow->splitDockWidget(datapointDock, eventDock, Qt::Horizontal);
}

void EssWorkspaceManager::createBottomPanel()
{
    // Create terminal dock
    QDockWidget *terminalDock = new QDockWidget(tr("Terminal"), m_mainWindow);
    terminalDock->setObjectName("TerminalDock");
    m_terminal = new EssTerminalWidget(terminalDock);
    terminalDock->setWidget(m_terminal);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, terminalDock);
    m_docks["Terminal"] = terminalDock;
    
    // Create console dock
    QDockWidget *consoleDock = new QDockWidget(tr("Output Console"), m_mainWindow);
    consoleDock->setObjectName("ConsoleDock");
    m_console = new EssOutputConsole(consoleDock);
    consoleDock->setWidget(m_console);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    m_docks["Console"] = consoleDock;
    
    // Tab them together
    m_mainWindow->tabifyDockWidget(terminalDock, consoleDock);
    terminalDock->raise(); // Make terminal the active tab
    
    // Register console
    EssConsoleManager::instance()->registerConsole("main", m_console);
}

void EssWorkspaceManager::createScriptEditor()
{
    // Script editor gets its own dock for flexibility
    QDockWidget *editorDock = new QDockWidget(tr("Script Editor"), m_mainWindow);
    editorDock->setObjectName("ScriptEditorDock");
    m_scriptEditor = new EssScriptEditorWidget(editorDock);
    editorDock->setWidget(m_scriptEditor);
    
    // Add to left area, but as a separate dock (not combined with control panel)
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, editorDock);
    m_docks["ScriptEditor"] = editorDock;
    
    // Tab it with the control panel for space efficiency
    m_mainWindow->tabifyDockWidget(m_docks["ControlPanel"], editorDock);
    m_docks["ControlPanel"]->raise(); // Control panel on top by default
}

void EssWorkspaceManager::connectSignals()
{
    // Terminal signals
    connect(m_terminal, &EssTerminalWidget::statusMessage,
            this, &EssWorkspaceManager::statusMessage);
    
    // Script editor signals
    connect(m_scriptEditor, &EssScriptEditorWidget::statusMessage,
            this, &EssWorkspaceManager::statusMessage);

  // When scripts are modified, optionally show in status bar
    connect(m_scriptEditor, &EssScriptEditorWidget::scriptModified,
            [this](EssScriptEditorWidget::ScriptType type, bool modified) {
                if (modified) {
                    QString scriptName = m_scriptEditor->scriptTypeToString(type);
                    emit statusMessage(QString("%1 script modified").arg(scriptName), 2000);
                }
            });
    
    // Host discovery signals
    connect(m_hostDiscovery, &EssHostDiscoveryWidget::hostSelected,
            [this](const QString &host) {
                emit statusMessage(QString("Selected host: %1").arg(host), 3000);
            });
    
    // Experiment control signals
    connect(m_experimentControl, &EssExperimentControlWidget::experimentStarted,
            [this]() { emit statusMessage("Experiment started", 3000); });
    connect(m_experimentControl, &EssExperimentControlWidget::experimentStopped,
            [this]() { emit statusMessage("Experiment stopped", 3000); });
    
    // Console messages
    m_console->logSystem("EssQt Workspace Initialized", "Workspace");
}

void EssWorkspaceManager::resetToDefaultLayout()
{
    // Hide all docks first
    for (auto dock : m_docks) {
        dock->setVisible(false);
    }
    
    // Remove all docks
    for (auto dock : m_docks) {
        m_mainWindow->removeDockWidget(dock);
    }
    
    // Re-add in default configuration
    // Left side: Control Panel and Script Editor (tabbed)
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ControlPanel"]);
    m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, m_docks["ScriptEditor"]);
    m_mainWindow->tabifyDockWidget(m_docks["ControlPanel"], m_docks["ScriptEditor"]);
    
    // Right side: Tables (side by side)
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["DatapointTable"]);
    m_mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_docks["EventTable"]);
    m_mainWindow->splitDockWidget(m_docks["DatapointTable"], m_docks["EventTable"], Qt::Horizontal);
    
    // Bottom: Terminal and Console (tabbed)
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Terminal"]);
    m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, m_docks["Console"]);
    m_mainWindow->tabifyDockWidget(m_docks["Terminal"], m_docks["Console"]);
    
    // Show all docks and set default active tabs
    for (auto dock : m_docks) {
        dock->setVisible(true);
    }
    m_docks["ControlPanel"]->raise();
    m_docks["Terminal"]->raise();
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

#include "EssMainWindow.h"
#include "core/EssApplication.h"
#include "core/EssConfig.h"
#include "core/EssCommandInterface.h"
#include "core/EssDataProcessor.h"

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QLabel>
#include <QMessageBox>
#include <QCloseEvent>
#include <QTextEdit>
#include <QDebug>
#include <QDockWidget>

#include "terminal/EssTerminalWidget.h"
#include "console/EssOutputConsole.h"
#include "dpoint_table/EssDatapointTableWidget.h"
#include "event_table/EssEventTableWidget.h"
#include "host_discovery/EssHostDiscoveryWidget.h"
#include "experiment_control/EssExperimentControlWidget.h"

EssMainWindow::EssMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_terminal(nullptr)
    , m_terminalDock(nullptr)
    , m_console(nullptr)
    , m_consoleDock(nullptr)
{
    setWindowTitle(QString("EssQt - ESS Control System"));

    setCentralWidget(nullptr);    

    createActions();
    createMenus();
    createStatusBar();
    createDockWidgets();
    
    readSettings();
    
    updateStatus("Ready", 5000);
}

EssMainWindow::~EssMainWindow()
{
}

void EssMainWindow::closeEvent(QCloseEvent *event)
{
    writeSettings();
    event->accept();
}

void EssMainWindow::createActions()
{
    // Actions are created directly in createMenus for simplicity
}

void EssMainWindow::createMenus()
{
    // File menu
    m_fileMenu = menuBar()->addMenu(tr("&File"));
    
    QAction *newAction = m_fileMenu->addAction(tr("&New Project..."));
    newAction->setShortcuts(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &EssMainWindow::onNew);
    
    QAction *openAction = m_fileMenu->addAction(tr("&Open Project..."));
    openAction->setShortcuts(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &EssMainWindow::onOpen);
    
    QAction *saveAction = m_fileMenu->addAction(tr("&Save"));
    saveAction->setShortcuts(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &EssMainWindow::onSave);
    
    QAction *saveAsAction = m_fileMenu->addAction(tr("Save &As..."));
    saveAsAction->setShortcuts(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &EssMainWindow::onSaveAs);
    
    m_fileMenu->addSeparator();
    
    QAction *preferencesAction = m_fileMenu->addAction(tr("&Preferences..."));
    preferencesAction->setShortcuts(QKeySequence::Preferences);
    connect(preferencesAction, &QAction::triggered, this, &EssMainWindow::onPreferences);
    
    m_fileMenu->addSeparator();
    
    QAction *quitAction = m_fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcuts(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    
    // Edit menu
    m_editMenu = menuBar()->addMenu(tr("&Edit"));
    // TODO: Add edit actions
    
    // View menu
    m_viewMenu = menuBar()->addMenu(tr("&View"));
    
    m_showTerminalAction = m_viewMenu->addAction(tr("&Terminal"));
    m_showTerminalAction->setCheckable(true);
    m_showTerminalAction->setChecked(true);
    m_showTerminalAction->setShortcut(QKeySequence("Ctrl+`"));
    connect(m_showTerminalAction, &QAction::triggered, this,
	    &EssMainWindow::onShowTerminal);    

    m_showConsoleAction = m_viewMenu->addAction(tr("&Output Console"));
    m_showConsoleAction->setCheckable(true);
    m_showConsoleAction->setChecked(true);
    m_showConsoleAction->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(m_showConsoleAction, &QAction::triggered, this,
	    &EssMainWindow::onShowConsole);

    m_showDatapointTableAction =
      m_viewMenu->addAction(tr("&Datapoint Monitor"));
    m_showDatapointTableAction->setCheckable(true);
    m_showDatapointTableAction->setChecked(true);
    m_showDatapointTableAction->setShortcut(QKeySequence("Ctrl+D"));
    connect(m_showDatapointTableAction, &QAction::triggered,
	    this, &EssMainWindow::onShowDatapointTable);
    
    m_showEventTableAction = m_viewMenu->addAction(tr("&Event Log"));
    m_showEventTableAction->setCheckable(true);
    m_showEventTableAction->setChecked(true);
    m_showEventTableAction->setShortcut(QKeySequence("Ctrl+E"));
    connect(m_showEventTableAction, &QAction::triggered,
	    this, &EssMainWindow::onShowEventTable);
    
    m_showHostDiscoveryAction = m_viewMenu->addAction(tr("&Host Discovery"));
    m_showHostDiscoveryAction->setCheckable(true);
    m_showHostDiscoveryAction->setChecked(true);
    m_showHostDiscoveryAction->setShortcut(QKeySequence("Ctrl+H"));
    connect(m_showHostDiscoveryAction, &QAction::triggered, this,
	    &EssMainWindow::onShowHostDiscovery);    

m_showExperimentControlAction = m_viewMenu->addAction(tr("E&xperiment Control"));
m_showExperimentControlAction->setCheckable(true);
m_showExperimentControlAction->setChecked(true);
m_showExperimentControlAction->setShortcut(QKeySequence("Ctrl+X"));
connect(m_showExperimentControlAction, &QAction::triggered,
        this, &EssMainWindow::onShowExperimentControl);
        
    m_viewMenu->addSeparator();
    QAction *resetLayoutAction = m_viewMenu->addAction(tr("&Reset Layout"));
    connect(resetLayoutAction, &QAction::triggered, this, &EssMainWindow::resetLayout);
    
    // Tools menu
    m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
    // TODO: Add tools (terminal, data viewer, etc.)
    
    // Help menu
    m_helpMenu = menuBar()->addMenu(tr("&Help"));
    
    QAction *aboutAction = m_helpMenu->addAction(tr("&About EssQt"));
    connect(aboutAction, &QAction::triggered, this, &EssMainWindow::onAbout);
    
    QAction *aboutQtAction = m_helpMenu->addAction(tr("About &Qt"));
    connect(aboutQtAction, &QAction::triggered, this, &EssMainWindow::onAboutQt);
}

void EssMainWindow::createStatusBar()
{
    // Main status message
    m_statusLabel = new QLabel("Ready");
    statusBar()->addWidget(m_statusLabel, 1);
    
    // Connection status
    m_connectionLabel = new QLabel("Not Connected");
    m_connectionLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_connectionLabel->setMinimumWidth(150);
    statusBar()->addPermanentWidget(m_connectionLabel);
    
    // Style the connection label
    updateConnectionStatus(false, "");
}

void EssMainWindow::createDockWidgets()
{
    // Create terminal dock
    m_terminalDock = new QDockWidget(tr("Terminal"), this);
    m_terminalDock->setObjectName("TerminalDock");
    
    m_terminal = new EssTerminalWidget(m_terminalDock);
    m_terminalDock->setWidget(m_terminal);
    
    addDockWidget(Qt::BottomDockWidgetArea, m_terminalDock);

    // Connect status bar updates to terminal
    connect(m_terminal, &EssTerminalWidget::statusMessage,
            this, &EssMainWindow::updateStatus);
    
    // Connect to command interface signals for status updates
    EssCommandInterface *cmdInterface =
      EssApplication::instance()->commandInterface();
    connect(cmdInterface, &EssCommandInterface::connected,
            this, &EssMainWindow::onConnected);
    connect(cmdInterface, &EssCommandInterface::disconnected,
            this, &EssMainWindow::onDisconnected);
    connect(cmdInterface, &EssCommandInterface::connectionError,
            this, &EssMainWindow::onConnectionError);
    
    // Connect to data processor for any UI updates
    EssDataProcessor *dataProc = EssApplication::instance()->dataProcessor();
    connect(dataProc, &EssDataProcessor::systemStatusUpdated,
            this, [this](const QString &status) {
                updateStatus(QString("System: %1").arg(status), 5000);
            });
    
    // Create output console dock
    m_consoleDock = new QDockWidget(tr("Output Console"), this);
    m_consoleDock->setObjectName("ConsoleDock");
    
    m_console = new EssOutputConsole(m_consoleDock);
    m_consoleDock->setWidget(m_console);
    
    // Add console to the right of terminal
    addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
    tabifyDockWidget(m_terminalDock, m_consoleDock);
    
    // Create datapoint table dock
    m_datapointTableDock = new QDockWidget(tr("Datapoint Monitor"), this);
    m_datapointTableDock->setObjectName("DatapointTableDock");
    
    m_datapointTable = new EssDatapointTableWidget(m_datapointTableDock);
    m_datapointTableDock->setWidget(m_datapointTable);
    
    addDockWidget(Qt::RightDockWidgetArea, m_datapointTableDock);
    
    m_datapointTable->setMaxRows(2000);
    m_datapointTable->setFilterPattern("");
    
    // Create event table dock
    m_eventTableDock = new QDockWidget(tr("Event Log"), this);
    m_eventTableDock->setObjectName("EventTableDock");
    
    m_eventTable = new EssEventTableWidget(m_eventTableDock);
    m_eventTableDock->setWidget(m_eventTable);
    
    addDockWidget(Qt::RightDockWidgetArea, m_eventTableDock);
    
    // Split datapoint and event tables side by side
    splitDockWidget(m_datapointTableDock, m_eventTableDock, Qt::Horizontal);

    // Create host discovery dock - ultra compact
    m_hostDiscoveryDock = new QDockWidget(tr("Hosts"), this);
    m_hostDiscoveryDock->setObjectName("HostDiscoveryDock");
    
    m_hostDiscovery = new EssHostDiscoveryWidget(m_hostDiscoveryDock);
    m_hostDiscoveryDock->setWidget(m_hostDiscovery);
    
    // Set size constraints for the ultra-compact combo box widget
    m_hostDiscovery->setMinimumHeight(32);   // Single line
    m_hostDiscovery->setMaximumHeight(32);   // Fixed height
    m_hostDiscoveryDock->setMaximumHeight(65); // Account for dock title bar
    
    // Create experiment control dock
    m_experimentControlDock = new QDockWidget(tr("Experiment Control"), this);
    m_experimentControlDock->setObjectName("ExperimentControlDock");
    
    m_experimentControl = new EssExperimentControlWidget(m_experimentControlDock);
    m_experimentControlDock->setWidget(m_experimentControl);
    
    // Add to left side - Host Discovery FIRST (on top)
    addDockWidget(Qt::LeftDockWidgetArea, m_hostDiscoveryDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_experimentControlDock);
    
    // Connect experiment control signals
    connect(m_experimentControl, &EssExperimentControlWidget::experimentStarted,
	    this, [this]() {
	      updateStatus("Experiment started", 3000);
	    });
    
    connect(m_experimentControl, &EssExperimentControlWidget::experimentStopped,
	    this, [this]() {
	      updateStatus("Experiment stopped", 3000);
	    });
    
    connect(m_experimentControl, &EssExperimentControlWidget::systemChanged,
	    this, [this](const QString &system) {
	      updateStatus(QString("System loaded: %1").arg(system), 3000);
	    });
    
    connect(m_experimentControl, &EssExperimentControlWidget::protocolChanged,
	    this, [this](const QString &protocol) {
        updateStatus(QString("Protocol loaded: %1").arg(protocol), 3000);
	    });
    
    connect(m_experimentControl, &EssExperimentControlWidget::variantChanged,
	    this, [this](const QString &variant) {
	      updateStatus(QString("Variant loaded: %1").arg(variant), 3000);
	    });
    
    connect(m_experimentControl, &EssExperimentControlWidget::experimentReset,
	    this, [this]() {
	      updateStatus("Experiment reset", 3000);
	    });
    
    // Connect dock visibility to menu action
    connect(m_experimentControlDock, &QDockWidget::visibilityChanged,
	    m_showExperimentControlAction, &QAction::setChecked);
        
    
    // Connect host discovery signals
    connect(m_hostDiscovery, &EssHostDiscoveryWidget::hostSelected,
	    this, [this](const QString &host) {
	      updateStatus(QString("Selected host: %1").arg(host), 3000);
	    });
    
    connect(m_hostDiscovery, &EssHostDiscoveryWidget::connectionStateChanged,
	    this, [this](bool connected, const QString &host) {
	      if (connected) {
                updateStatus(QString("Connected to %1").arg(host), 5000);
	      } else {
                updateStatus("Disconnected", 3000);
	      }
	    });
    
    // Connect dock visibility to menu action
    connect(m_hostDiscoveryDock, &QDockWidget::visibilityChanged,
	    m_showHostDiscoveryAction, &QAction::setChecked);
    
    
    // Register console with manager
    EssConsoleManager::instance()->registerConsole("main", m_console);
    
    // Show welcome messages
    m_console->logSystem("EssQt Application Started", "Main");
    m_console->logInfo("Output console ready", "Console");
    m_console->logInfo("Event log ready", "EventLog");
    m_console->logSuccess("All systems initialized", "Startup");
    
    // Connect dock visibility changes to menu actions
    connect(m_terminalDock, &QDockWidget::visibilityChanged,
            m_showTerminalAction, &QAction::setChecked);
    connect(m_consoleDock, &QDockWidget::visibilityChanged,
            m_showConsoleAction, &QAction::setChecked);
    connect(m_datapointTableDock, &QDockWidget::visibilityChanged,
            m_showDatapointTableAction, &QAction::setChecked);
    connect(m_eventTableDock, &QDockWidget::visibilityChanged,
            m_showEventTableAction, &QAction::setChecked);
}

void EssMainWindow::resetLayout()
{
    // Hide all docks first
    m_terminalDock->setVisible(false);
    m_consoleDock->setVisible(false);
    m_datapointTableDock->setVisible(false);
    m_eventTableDock->setVisible(false);
    m_hostDiscoveryDock->setVisible(false);
    m_experimentControlDock->setVisible(false);
    
    // Remove all docks
    removeDockWidget(m_terminalDock);
    removeDockWidget(m_consoleDock);
    removeDockWidget(m_datapointTableDock);
    removeDockWidget(m_eventTableDock);
    removeDockWidget(m_hostDiscoveryDock);
    removeDockWidget(m_experimentControlDock);
    
    // Re-add in desired configuration
    addDockWidget(Qt::BottomDockWidgetArea, m_terminalDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
    tabifyDockWidget(m_terminalDock, m_consoleDock);
    
    addDockWidget(Qt::RightDockWidgetArea, m_datapointTableDock);
    addDockWidget(Qt::RightDockWidgetArea, m_eventTableDock);
    splitDockWidget(m_datapointTableDock, m_eventTableDock, Qt::Horizontal);

    // Host Discovery on top, Experiment Control below
    addDockWidget(Qt::LeftDockWidgetArea, m_hostDiscoveryDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_experimentControlDock);
    
    // Show all docks
    m_terminalDock->setVisible(true);
    m_consoleDock->setVisible(true);
    m_datapointTableDock->setVisible(true);
    m_eventTableDock->setVisible(true);
    m_hostDiscoveryDock->setVisible(true);
    m_experimentControlDock->setVisible(true);
    
    // Update menu checkmarks
    m_showTerminalAction->setChecked(true);
    m_showConsoleAction->setChecked(true);
    m_showDatapointTableAction->setChecked(true);
    m_showEventTableAction->setChecked(true);
    m_showHostDiscoveryAction->setChecked(true);    
    m_showExperimentControlAction->setChecked(true);
}
void EssMainWindow::readSettings()
{
    EssConfig *config = EssApplication::instance()->config();
    
    QByteArray geometry = config->windowGeometry();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    } else {
        // Default size
        resize(1200, 800);
    }
    
    QByteArray state = config->windowState();
    if (!state.isEmpty()) {
        restoreState(state);
    }
}

void EssMainWindow::writeSettings()
{
    EssConfig *config = EssApplication::instance()->config();
    config->setWindowGeometry(saveGeometry());
    config->setWindowState(saveState());
    config->sync();
}

// Slot implementations
void EssMainWindow::onNew()
{
    updateStatus("New project functionality not yet implemented", 3000);
}

void EssMainWindow::onOpen()
{
    updateStatus("Open project functionality not yet implemented", 3000);
}

void EssMainWindow::onSave()
{
    updateStatus("Save functionality not yet implemented", 3000);
}

void EssMainWindow::onSaveAs()
{
    updateStatus("Save As functionality not yet implemented", 3000);
}

void EssMainWindow::onPreferences()
{
    updateStatus("Preferences dialog not yet implemented", 3000);
}

void EssMainWindow::onAbout()
{
    QMessageBox::about(this, tr("About EssQt"),
        tr("<h3>EssQt %1</h3>"
           "<p>A modern Qt-based frontend for the ESS "
           "(Experiment State System) control system.</p>"
           "<p>Built with Qt %2</p>")
        .arg(EssApplication::Version)
        .arg(QT_VERSION_STR));
}

void EssMainWindow::onAboutQt()
{
    QApplication::aboutQt();
}

void EssMainWindow::updateStatus(const QString &message, int timeout)
{
    statusBar()->showMessage(message, timeout);
}

void EssMainWindow::onShowTerminal()
{
    if (m_terminalDock) {
        m_terminalDock->setVisible(!m_terminalDock->isVisible());
    }
}

void EssMainWindow::onShowConsole()
{
    if (m_consoleDock) {
        m_consoleDock->setVisible(!m_consoleDock->isVisible());
    }
}

void EssMainWindow::onShowDatapointTable()
{
    if (m_datapointTableDock) {
        m_datapointTableDock->setVisible(!m_datapointTableDock->isVisible());
    }
}

void EssMainWindow::onShowEventTable()
{
    if (m_eventTableDock) {
        m_eventTableDock->setVisible(!m_eventTableDock->isVisible());
    }
}

void EssMainWindow::onShowHostDiscovery()
{
    if (m_hostDiscoveryDock) {
        m_hostDiscoveryDock->setVisible(!m_hostDiscoveryDock->isVisible());
    }
}

void EssMainWindow::onShowExperimentControl()
{
    if (m_experimentControlDock) {
        m_experimentControlDock->setVisible(!m_experimentControlDock->isVisible());
    }
}

void EssMainWindow::onConnected(const QString &host)
{
    updateConnectionStatus(true, host);
    updateStatus(QString("Connected to %1").arg(host), 3000);
}

void EssMainWindow::onDisconnected()
{
    updateConnectionStatus(false, "");
    updateStatus("Disconnected", 3000);
}

void EssMainWindow::onConnectionError(const QString &error)
{
    updateConnectionStatus(false, "");
    updateStatus(QString("Connection error: %1").arg(error), 5000);
}

void EssMainWindow::updateConnectionStatus(bool connected, const QString &host)
{
    if (connected) {
        m_connectionLabel->setText(QString(" Connected: %1 ").arg(host));
        m_connectionLabel->setStyleSheet(
            "QLabel { "
            "  background-color: #2d7d2d; "  // Dark green
            "  color: white; "
            "  font-weight: bold; "
            "  padding: 2px 8px; "
            "  border-radius: 3px; "
            "}"
        );
    } else {
        m_connectionLabel->setText(" Not Connected ");
        m_connectionLabel->setStyleSheet(
            "QLabel { "
            "  background-color: #7d2d2d; "  // Dark red
            "  color: white; "
            "  padding: 2px 8px; "
            "  border-radius: 3px; "
            "}"
        );
    }
}

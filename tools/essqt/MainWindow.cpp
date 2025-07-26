// MainWindow.cpp implementation
#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMenuBar>
#include <QCloseEvent>
#include "dlfuncs.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupComponents();
    setupDocks();
    setupMenus();
    connectSignals();
}

void MainWindow::setupComponents() {
    setupStatusBar();

    // Initialize Tcl interpreter first
    try {
        char* argv[] = {(char*)"essgui", nullptr};
        tclInterpreter = new TclInterp(1, argv);
        setupTclCommands();
        qDebug() << "✓ Tcl interpreter initialized";
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Tcl Initialization Error", 
                           QString("Failed to initialize Tcl: %1").arg(e.what()));
        tclInterpreter = nullptr;
    }  
    
    // Initialize dock manager
    dockManager = new DockManager(this, this);
    
    // Initialize connection manager
    connectionManager = new ConnectionManager(this);
    
    // Create widgets that will be managed by docks
    hostDiscovery = new HostDiscoveryWidget();
    essControl = new EssControlWidget();
    editor = new CodeEditor(this);
    
    qDebug() << "Creating TclConsoleWidget...";
    tclConsole = new TclConsoleWidget(this);
    qDebug() << "TclConsoleWidget created:" << tclConsole;
    
    qDebug() << "Creating DgTableTabs...";
    dgTables = new DgTableTabs(this);
    qDebug() << "DgTableTabs created:" << dgTables;
    
    // Create terminal widget
    terminalWidget = new QWidget();
    auto* layout = new QVBoxLayout(terminalWidget);
    
    terminalOutput = new QPlainTextEdit();
    terminalOutput->setReadOnly(true);
    
    commandInput = new QLineEdit();
    sendButton = new QPushButton("Send");
    
    auto* inputLayout = new QHBoxLayout();
    inputLayout->addWidget(commandInput);
    inputLayout->addWidget(sendButton);
    
    layout->addWidget(terminalOutput);
    layout->addLayout(inputLayout);
    
    // Terminal client
    client = new TerminalClient(this);
    client->connectToServer(server, port);
}

void MainWindow::setupDocks() {
    // Create docks using the manager - order matters for left panel stacking
    dockManager->createDock(DockType::EssControl, essControl);
    dockManager->createDock(DockType::HostDiscovery, hostDiscovery);
    dockManager->createDock(DockType::DataViewer, dgTables);
    dockManager->createDock(DockType::CodeEditor, editor);
    dockManager->createDock(DockType::TclConsole, tclConsole);
    dockManager->createDock(DockType::Terminal, terminalWidget);
    
    // Apply default layout
    dockManager->setDefaultLayout();
}

void MainWindow::setupMenus() {
    // File Menu
    auto* fileMenu = menuBar()->addMenu("&File");
    
    auto* newProjectAction = fileMenu->addAction("&New Project...");
    newProjectAction->setShortcut(QKeySequence::New);
    connect(newProjectAction, &QAction::triggered, this, &MainWindow::newProject);
    
    auto* openProjectAction = fileMenu->addAction("&Open Project...");
    openProjectAction->setShortcut(QKeySequence::Open);
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);
    
    auto* saveProjectAction = fileMenu->addAction("&Save Project");
    saveProjectAction->setShortcut(QKeySequence::Save);
    connect(saveProjectAction, &QAction::triggered, this, &MainWindow::saveProject);
    
    fileMenu->addSeparator();
    
    auto* importDataAction = fileMenu->addAction("&Import Data...");
    connect(importDataAction, &QAction::triggered, this, &MainWindow::importData);
    
    auto* exportDataAction = fileMenu->addAction("&Export Data...");
    connect(exportDataAction, &QAction::triggered, this, &MainWindow::exportData);
    
    fileMenu->addSeparator();
    
    auto* preferencesAction = fileMenu->addAction("&Preferences...");
    preferencesAction->setShortcut(QKeySequence::Preferences);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);
    
    fileMenu->addSeparator();
    
    auto* exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // Connection Menu
    auto* connectionMenu = menuBar()->addMenu("&Connection");
    
    auto* connectAction = connectionMenu->addAction("&Connect to Host...");
    connectAction->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(connectAction, &QAction::triggered, this, &MainWindow::showConnectDialog);
    
    auto* disconnectAction = connectionMenu->addAction("&Disconnect");
    disconnectAction->setShortcut(QKeySequence("Ctrl+Shift+D"));
    connect(disconnectAction, &QAction::triggered, this, &MainWindow::disconnectFromHost);
    
    connectionMenu->addSeparator();
    
    auto* connectionSettingsAction = connectionMenu->addAction("Connection &Settings...");
    connect(connectionSettingsAction, &QAction::triggered, this, &MainWindow::showConnectionSettings);

    // Experiment Menu
    auto* experimentMenu = menuBar()->addMenu("&Experiment");
    
    auto* startExpAction = experimentMenu->addAction("&Start Experiment");
    startExpAction->setShortcut(QKeySequence("F5"));
    startExpAction->setIcon(QIcon::fromTheme("media-playback-start"));
    connect(startExpAction, &QAction::triggered, this, &MainWindow::onStartRequested);
    
    auto* stopExpAction = experimentMenu->addAction("S&top Experiment");
    stopExpAction->setShortcut(QKeySequence("F6"));
    stopExpAction->setIcon(QIcon::fromTheme("media-playback-stop"));
    connect(stopExpAction, &QAction::triggered, this, &MainWindow::onStopRequested);
    
    auto* resetExpAction = experimentMenu->addAction("&Reset Experiment");
    resetExpAction->setShortcut(QKeySequence("F7"));
    connect(resetExpAction, &QAction::triggered, this, &MainWindow::onResetRequested);
    
    experimentMenu->addSeparator();
    
    auto* loadSystemAction = experimentMenu->addAction("&Load System...");
    connect(loadSystemAction, &QAction::triggered, this, &MainWindow::showSystemSelector);
    
    auto* reloadCurrentAction = experimentMenu->addAction("&Reload Current System");
    reloadCurrentAction->setShortcut(QKeySequence("F9"));
    connect(reloadCurrentAction, &QAction::triggered, this, &MainWindow::reloadSystem);

    // Tools Menu
    auto* toolsMenu = menuBar()->addMenu("&Tools");
    
    auto* dataViewerAction = toolsMenu->addAction("&Data Viewer");
    dataViewerAction->setShortcut(QKeySequence("Ctrl+D"));
    connect(dataViewerAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::DataViewer);
    });
    
    auto* terminalAction = toolsMenu->addAction("&Terminal");
    terminalAction->setShortcut(QKeySequence("Ctrl+T"));
    connect(terminalAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::Terminal);
    });
    
    auto* codeEditorAction = toolsMenu->addAction("&Code Editor");
    codeEditorAction->setShortcut(QKeySequence("Ctrl+E"));
    connect(codeEditorAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::CodeEditor);
    });
    
    auto* tclConsoleAction = toolsMenu->addAction("&Tcl Console");
    tclConsoleAction->setShortcut(QKeySequence("Ctrl+Shift+T"));
    connect(tclConsoleAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::TclConsole);
    });
    
    toolsMenu->addSeparator();
    
    auto* performanceAction = toolsMenu->addAction("&Performance Monitor");
    connect(performanceAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::PerformanceAnalyzer);
    });
    
    auto* logViewerAction = toolsMenu->addAction("&Log Viewer");
    connect(logViewerAction, &QAction::triggered, [this]() {
        dockManager->showDock(DockType::LogViewer);
    });

    // View Menu (your existing dock management)
    auto* viewMenu = menuBar()->addMenu("&View");
    dockManager->setupViewMenu(viewMenu);
    
    viewMenu->addSeparator();
    
    auto* fullscreenAction = viewMenu->addAction("&Full Screen");
    fullscreenAction->setShortcut(QKeySequence::FullScreen);
    fullscreenAction->setCheckable(true);
    connect(fullscreenAction, &QAction::triggered, this, [this, fullscreenAction](bool checked) {
        if (checked) {
            showFullScreen();
        } else {
            showNormal();
        }
    });

    // Help Menu
    auto* helpMenu = menuBar()->addMenu("&Help");
    
    auto* userGuideAction = helpMenu->addAction("&User Guide");
    userGuideAction->setShortcut(QKeySequence::HelpContents);
    connect(userGuideAction, &QAction::triggered, this, &MainWindow::showUserGuide);
    
    auto* keyboardShortcutsAction = helpMenu->addAction("&Keyboard Shortcuts");
    connect(keyboardShortcutsAction, &QAction::triggered, this, &MainWindow::showKeyboardShortcuts);
    
    helpMenu->addSeparator();
    
    auto* aboutAction = helpMenu->addAction("&About EssQt");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    
    auto* aboutQtAction = helpMenu->addAction("About &Qt");
    connect(aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::setupStatusBar() {
    // Create status bar with permanent widgets
    QStatusBar* statusBarWidget = this->statusBar();
    
    // Connection status (left side)
    connectionStatusLabel = new QLabel("Disconnected");
    connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    statusBarWidget->addWidget(connectionStatusLabel);
    
    // Add separator
    auto* separator1 = new QFrame();
    separator1->setFrameStyle(QFrame::VLine | QFrame::Sunken);
    statusBarWidget->addWidget(separator1);
    
    // System status
    systemStatusLabel = new QLabel("No System");
    statusBarWidget->addWidget(systemStatusLabel);
    
    // Add separator
    auto* separator2 = new QFrame();
    separator2->setFrameStyle(QFrame::VLine | QFrame::Sunken);
    statusBarWidget->addWidget(separator2);
    
    // Observation status
    observationStatusLabel = new QLabel("Obs: 0/0");
    statusBarWidget->addWidget(observationStatusLabel);
    
    // Stretch to push permanent widgets to the right
    statusBarWidget->addWidget(new QWidget(), 1);
    
    // Progress bar (permanent, right side)
    progressBar = new QProgressBar();
    progressBar->setVisible(false);
    progressBar->setMaximumWidth(200);
    statusBarWidget->addPermanentWidget(progressBar);
    
    // System info (permanent, far right)
    auto* systemInfoLabel = new QLabel("EssQt v1.0");
    systemInfoLabel->setStyleSheet("QLabel { color: gray; }");
    statusBarWidget->addPermanentWidget(systemInfoLabel);
}

QString MainWindow::evaluateTcl(const QString& command) {
    if (!tclInterpreter) {
        return "Error: Tcl interpreter not initialized";
    }
    
    try {
        return QString::fromStdString(tclInterpreter->eval(command.toUtf8().constData()));
    } catch (const std::exception& e) {
        return QString("Tcl Error: %1").arg(e.what());
    }
}

bool MainWindow::evaluateTclWithResult(const QString& command, QString& result) {
    if (!tclInterpreter) {
        result = "Error: Tcl interpreter not initialized";
        return false;
    }
    
    try {
        std::string resultStr;
        int returnCode = tclInterpreter->eval(command.toUtf8().constData(), resultStr);
        result = QString::fromStdString(resultStr);
        return (returnCode == TCL_OK);
    } catch (const std::exception& e) {
        result = QString("Tcl Error: %1").arg(e.what());
        return false;
    }
}

void MainWindow::setupTclCommands() {
    if (!tclInterpreter) return;
    
    // Register Qt-specific Tcl commands
    registerQtTclCommands();
    
    // Your existing Tcl scripts
    QString initScript = R"(
        # ESS GUI Tcl initialization
        proc log_message {msg} {
            puts "GUI: $msg"
        }
        
        proc get_current_time {} {
            return [clock format [clock seconds]]
        }
        
        # Add your custom Tcl procedures here
        log_message "ESS GUI Tcl environment initialized"
    )";
    
    evaluateTcl(initScript);
}

void MainWindow::registerQtTclCommands() {
    if (!tclInterpreter) return;
    
    Tcl_Interp* interp = tclInterpreter->interp();

    qDebug() << "adding tcl commands including dg_view";
    
    // Register the dg_view command - displays DYN_GROUP in Qt data viewer
    Tcl_CreateObjCommand(interp, "dg_view", 
                        (Tcl_ObjCmdProc*)dg_view_func, 
                        (ClientData)this, NULL);
    
    // Register the print command - outputs to Qt terminal
    Tcl_CreateObjCommand(interp, "print", 
                        (Tcl_ObjCmdProc*)print_func, 
                        (ClientData)this, NULL);
    
    // Register qt_message command - shows Qt message boxes
    Tcl_CreateObjCommand(interp, "qt_message", 
                        (Tcl_ObjCmdProc*)qt_message_func, 
                        (ClientData)this, NULL);
    
    // Register load_data command - loads data files into viewer
    Tcl_CreateObjCommand(interp, "load_data", 
                        (Tcl_ObjCmdProc*)load_data_func, 
                        (ClientData)this, NULL);
}

// Implementation of dg_view command - similar to your FLTK version
int MainWindow::dg_view_func(ClientData data, Tcl_Interp *interp, 
                            int objc, Tcl_Obj *const objv[]) {
    MainWindow* mainWindow = static_cast<MainWindow*>(data);
    DYN_GROUP* dg = nullptr;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "dg_name");
        return TCL_ERROR;
    }
    
    char* groupName = Tcl_GetString(objv[1]);
    
    // Try to find the DYN_GROUP in the Tcl interpreter
    if (tclFindDynGroup(interp, groupName, &dg) == TCL_OK && dg) {
        // Display the DYN_GROUP in the Qt data viewer
        mainWindow->showDynGroupInViewer(dg, QString::fromUtf8(groupName));
        
        // Show the data viewer dock
        mainWindow->dockManager->showDock(DockType::DataViewer);
        
        return TCL_OK;
    }
    
    return TCL_ERROR;
}

// Implementation of print command - outputs to Qt terminal
int MainWindow::print_func(ClientData data, Tcl_Interp *interp, 
                          int objc, Tcl_Obj *const objv[]) {
    MainWindow* mainWindow = static_cast<MainWindow*>(data);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "string");
        return TCL_ERROR;
    }
    
    QString message = QString::fromUtf8(Tcl_GetString(objv[1]));
    mainWindow->terminalOutput->appendPlainText(message);
    
    return TCL_OK;
}

// Implementation of qt_message command - shows Qt message boxes
int MainWindow::qt_message_func(ClientData data, Tcl_Interp *interp, 
                               int objc, Tcl_Obj *const objv[]) {
    MainWindow* mainWindow = static_cast<MainWindow*>(data);
    
    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "message ?title?");
        return TCL_ERROR;
    }
    
    QString message = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString title = (objc == 3) ? QString::fromUtf8(Tcl_GetString(objv[2])) : "Message";
    
    QMessageBox::information(mainWindow, title, message);
    
    return TCL_OK;
}

// Implementation of load_data command - loads data files
int MainWindow::load_data_func(ClientData data, Tcl_Interp *interp, 
                              int objc, Tcl_Obj *const objv[]) {
    MainWindow* mainWindow = static_cast<MainWindow*>(data);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        return TCL_ERROR;
    }
    
    QString filename = QString::fromUtf8(Tcl_GetString(objv[1]));
    mainWindow->loadDataFile(filename);
    
    return TCL_OK;
}

void MainWindow::onTclCommandRequested(const QString& command) {
    QString result = evaluateTcl(command);
    terminalOutput->appendPlainText(QString("Tcl> %1").arg(command));
    if (!result.isEmpty()) {
        terminalOutput->appendPlainText(result);
    }
}

void MainWindow::loadDataFile(const QString& filename) {
    if (filename.isEmpty()) return;
    
    terminalOutput->appendPlainText(QString("Loading data file: %1").arg(filename));
    
    // Load in data viewer
    int tabIndex = dgTables->addFromFile(filename);
    
    if (tabIndex >= 0) {
        // Show the data viewer dock
        dockManager->showDock(DockType::DataViewer);
        
        // Get the loaded DYN_GROUP and put it in Tcl for analysis
        DgTableWidget* table = dgTables->tableAt(tabIndex);
        if (table && table->dynGroup()) {
            DYN_GROUP* dg = table->dynGroup();
            
            // Put the DYN_GROUP into both Tcl environments
            if (tclInterpreter) {
                int result = tclInterpreter->tclPutGroup(dg);
                if (result == TCL_OK) {
                    terminalOutput->appendPlainText(QString("✓ Data available in Tcl as '%1'").arg(DYN_GROUP_NAME(dg)));
                } else {
                    terminalOutput->appendPlainText("✗ Failed to put data in Tcl interpreter");
                }
            }
            
            // Also put it in the Tcl console
            if (tclConsole) {
                tclConsole->putDynGroup(dg);
                tclConsole->evaluateCommand(QString("puts \"Data loaded: %1 (%2 lists)\"")
                                           .arg(DYN_GROUP_NAME(dg))
                                           .arg(DYN_GROUP_N(dg)));
            }
        }
    } else {
        terminalOutput->appendPlainText("✗ Failed to load data file");
    }
}

void MainWindow::showDynGroupInViewer(DYN_GROUP* dg, const QString& name) {
    if (!dg) return;
    
    QString displayName = name.isEmpty() ? 
        (DYN_GROUP_NAME(dg) ? QString::fromUtf8(DYN_GROUP_NAME(dg)) : "Untitled") : 
        name;
    
    int tabIndex = dgTables->addDynGroup(dg, displayName);
    
    if (tabIndex >= 0) {
        // Show the data viewer dock
        dockManager->showDock(DockType::DataViewer);
        terminalOutput->appendPlainText(QString("✓ Data displayed in viewer: %1").arg(displayName));
    }
}

void MainWindow::connectSignals() {
    // Connection manager signals
    connect(connectionManager, &ConnectionManager::connected,
            this, &MainWindow::onHostConnected);
    connect(connectionManager, &ConnectionManager::disconnected,
            this, &MainWindow::onHostDisconnected);
    connect(connectionManager, &ConnectionManager::receivedEvent,
            this, &MainWindow::handleEvent);
    connect(connectionManager, &ConnectionManager::errorOccurred,
            this, &MainWindow::handleConnectionError);

    connect(connectionManager, &ConnectionManager::connected,
            hostDiscovery, &HostDiscoveryWidget::onHostConnected);
    connect(connectionManager, &ConnectionManager::disconnected,
            hostDiscovery, &HostDiscoveryWidget::onHostDisconnected);
    
    // ESS Control signals
    connect(essControl, &EssControlWidget::subjectChanged,
            this, &MainWindow::onSubjectChanged);
    connect(essControl, &EssControlWidget::systemChanged,
            this, &MainWindow::onSystemChanged);
    connect(essControl, &EssControlWidget::protocolChanged,
            this, &MainWindow::onProtocolChanged);
    connect(essControl, &EssControlWidget::variantChanged,
            this, &MainWindow::onVariantChanged);
    connect(essControl, &EssControlWidget::startRequested,
            this, &MainWindow::onStartRequested);
    connect(essControl, &EssControlWidget::stopRequested,
            this, &MainWindow::onStopRequested);
    connect(essControl, &EssControlWidget::resetRequested,
            this, &MainWindow::onResetRequested);
    connect(essControl, &EssControlWidget::reloadSystemRequested,
            this, &MainWindow::reloadSystem);
    connect(essControl, &EssControlWidget::reloadProtocolRequested,
            this, &MainWindow::reloadProtocol);
    connect(essControl, &EssControlWidget::reloadVariantRequested,
            this, &MainWindow::reloadVariant);
    connect(essControl, &EssControlWidget::saveSettingsRequested,
            this, &MainWindow::saveSettings);
    connect(essControl, &EssControlWidget::resetSettingsRequested,
            this, &MainWindow::resetSettings);

    // Host Discovery signals
    connect(hostDiscovery, &HostDiscoveryWidget::connectRequested,
            this, &MainWindow::connectToHost);
    connect(hostDiscovery, &HostDiscoveryWidget::disconnectRequested,
            this, &MainWindow::disconnectFromHost);
    
    // Data Viewer signals
    connect(dgTables, &DgTableTabs::dataLoaded, this, [this](int tabIndex, const QString& filename) {
        terminalOutput->appendPlainText(QString("✓ Data loaded in tab %1: %2").arg(tabIndex).arg(filename));
    });
    
    // Terminal signals
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendCommand);
    connect(commandInput, &QLineEdit::returnPressed, this, &MainWindow::sendCommand);
    connect(client, &TerminalClient::messageReceived, this, &MainWindow::handleResponse);
    connect(client, &TerminalClient::errorOccurred, this, &MainWindow::handleError);
    connect(editor, &CodeEditor::sendText, client, &TerminalClient::sendMessage);
    
    // Dock manager signals
    connect(dockManager, &DockManager::dockVisibilityChanged,
            this, &MainWindow::onDockVisibilityChanged);
}

// Update existing connection methods to update status bar
void MainWindow::onHostConnected(const QString& host) {
    terminalOutput->appendPlainText(QString("✓ Connected to host: %1").arg(host));
    setWindowTitle(QString("EssQt - Connected to %1").arg(host));
    
    // Update status bar
    connectionStatusLabel->setText(QString("Connected to %1").arg(host));
    connectionStatusLabel->setStyleSheet("QLabel { color: green; font-weight: bold; }");
    this->statusBar()->showMessage(QString("Successfully connected to %1").arg(host), 3000);
    
    // Update HostDiscoveryWidget
    if (hostDiscovery) {
        hostDiscovery->onHostConnected(host);
    }
}

void MainWindow::onHostDisconnected() {
    terminalOutput->appendPlainText("✗ Disconnected from host");
    setWindowTitle("EssQt - Disconnected");
    
    // Update status bar
    connectionStatusLabel->setText("Disconnected");
    connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    systemStatusLabel->setText("No System");
    observationStatusLabel->setText("Obs: 0/0");
    this->statusBar()->showMessage("Disconnected from host", 3000);
    
    // Update HostDiscoveryWidget to show disconnected state
    if (hostDiscovery) {
        hostDiscovery->onHostDisconnected();
    }
    
    clearWidgets();
}

void MainWindow::handleConnectionError(const QString& error) {
    terminalOutput->appendPlainText(QString("Connection Error: %1").arg(error));
    QMessageBox::warning(this, "Connection Error", error);
}

void MainWindow::connectToHost(const QString& host) {
    terminalOutput->appendPlainText(QString("Attempting to connect to %1...").arg(host));
    
    if (!connectionManager) {
        qDebug() << "ConnectionManager is null!";
        terminalOutput->appendPlainText("Error: ConnectionManager not initialized");
        return;
    }
    
    bool result = connectionManager->connectToHost(host);
    
    if (!result) {
        terminalOutput->appendPlainText(QString("Failed to initiate connection to %1").arg(host));
        QMessageBox::warning(this, "Connection Failed", 
                           QString("Failed to connect to %1").arg(host));
    } else {
        terminalOutput->appendPlainText(QString("Connection initiated to %1...").arg(host));
    }
}

void MainWindow::disconnectFromHost() {
    connectionManager->disconnectFromHost();
}

void MainWindow::clearWidgets() {
    // Clear ESS Control widget state
    essControl->setSystemStatus("Disconnected");
    essControl->setCurrentSubject("");
    essControl->setObservationCount("0/0");
    essControl->setObservationActive(false);
    
    // Clear system configuration dropdowns
    essControl->systemConfig()->setSystemList(QStringList());
    essControl->systemConfig()->setProtocolList(QStringList());
    essControl->systemConfig()->setVariantList(QStringList());
    
    // Reset current selections
    essControl->systemConfig()->setCurrentSystem("");
    essControl->systemConfig()->setCurrentProtocol("");
    essControl->systemConfig()->setCurrentVariant("");
}

// Event Handling
void MainWindow::handleEvent(const QString& msg) {
    DservEventParser parser;
    auto eventOpt = parser.parse(msg);
    if (!eventOpt) return;
    
    const auto& event = *eventOpt;
    
    // Block signals to prevent callbacks during programmatic updates
    essControl->blockSignals(true);
    
    // Update ESS Control widget based on events
    if (event.name == "ess/systems") {
        QStringList systems = event.data.toString().split(' ', Qt::SkipEmptyParts);
        essControl->systemConfig()->setSystemList(systems);
    }
    else if (event.name == "ess/protocols") {
        QStringList protocols = event.data.toString().split(' ', Qt::SkipEmptyParts);
        essControl->systemConfig()->setProtocolList(protocols);
    }
    else if (event.name == "ess/variants") {
        QStringList variants = event.data.toString().split(' ', Qt::SkipEmptyParts);
        essControl->systemConfig()->setVariantList(variants);
    }
    else if (event.name == "ess/system") {
        essControl->systemConfig()->setCurrentSystem(event.data.toString());
    }
    else if (event.name == "ess/protocol") {
        essControl->systemConfig()->setCurrentProtocol(event.data.toString());
    }
    else if (event.name == "ess/variant") {
        essControl->systemConfig()->setCurrentVariant(event.data.toString());
    }
    else if (event.name == "ess/state") {
        QString status = event.data.toString();
        systemStatusLabel->setText(QString("System: %1").arg(status));
        
        if (status == "Running") {
          systemStatusLabel->setStyleSheet("QLabel { color: #28c814; font-weight: bold; }");
        } else if (status == "Stopped") {
          systemStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
        } else {
          systemStatusLabel->setStyleSheet("QLabel { color: black; font-weight: bold; }");
        }
    }
    else if (event.name == "ess/subject") {
        // Update subject list if needed, then set current
        essControl->setCurrentSubject(event.data.toString());
    }
    else if (event.name == "ess/obs_id" || event.name == "ess/obs_total") {
        // Get current values - obs_id and obs_total are sent as integers
        static int currentObsId = 0;
        static int currentObsTotal = 0;
        
        if (event.name == "ess/obs_id") {
            currentObsId = event.data.toInt();
        } else if (event.name == "ess/obs_total") {
            currentObsTotal = event.data.toInt();
        }
        
        // Update observation status display (1-based indexing for display)
        observationStatusLabel->setText(QString("Obs: %1/%2").arg(currentObsId + 1).arg(currentObsTotal));
        
        // Update progress bar if experiment is running and we have a valid total
        if (currentObsTotal > 0) {
            int percentage = ((currentObsId + 1) * 100) / currentObsTotal;
            if (progressBar->isVisible()) {
                progressBar->setValue(percentage);
            }
        }
    }
    else if (event.name == "ess/in_obs") {
        essControl->setObservationActive(event.data.toString() == "1");
    }
    else {
        // Log unhandled events for debugging
        terminalOutput->appendPlainText(QString("Event: %1 = %2")
                                       .arg(event.name)
                                       .arg(event.data.toString()));
    }
    
    // Re-enable signals
    essControl->blockSignals(false);
}

// ESS Control Command Slots
void MainWindow::onSubjectChanged(const QString& subject) {
    QString cmd = QString("ess::set_subject %1").arg(subject);
    QString response;
    if (connectionManager->sendEssCommand(cmd, response)) {
        terminalOutput->appendPlainText(QString("Subject changed to: %1").arg(subject));
    } else {
        terminalOutput->appendPlainText("Failed to change subject");
    }
}

void MainWindow::onSystemChanged(const QString& system) {
    QString cmd = QString("ess::load_system %1").arg(system);
    QString response;
    if (connectionManager->sendEssCommand(cmd, response)) {
        terminalOutput->appendPlainText(QString("System changed to: %1").arg(system));
    } else {
        terminalOutput->appendPlainText("Failed to change system");
    }
}

void MainWindow::onProtocolChanged(const QString& protocol) {
    QString cmd = QString("ess::load_system %1 %2")
                     .arg(essControl->systemConfig()->currentSystem())
                     .arg(protocol);
    QString response;
    if (connectionManager->sendEssCommand(cmd, response)) {
        terminalOutput->appendPlainText(QString("Protocol changed to: %1").arg(protocol));
    } else {
        terminalOutput->appendPlainText("Failed to change protocol");
    }
}

void MainWindow::onVariantChanged(const QString& variant) {
    QString cmd = QString("ess::load_system %1 %2 %3")
                     .arg(essControl->systemConfig()->currentSystem())
                     .arg(essControl->systemConfig()->currentProtocol())
                     .arg(variant);
    QString response;
    if (connectionManager->sendEssCommand(cmd, response)) {
        terminalOutput->appendPlainText(QString("Variant changed to: %1").arg(variant));
    } else {
        terminalOutput->appendPlainText("Failed to change variant");
    }
}

void MainWindow::onStartRequested() {
    QString response;
    if (connectionManager->sendEssCommand("ess::start", response)) {
        terminalOutput->appendPlainText("✓ Start command sent");
        this->statusBar()->showMessage("Experiment started", 2000);
        showProgress("Starting experiment...", 0);
    } else {
        terminalOutput->appendPlainText("✗ Failed to send start command");
        this->statusBar()->showMessage("Failed to start experiment", 3000);
    }
}

void MainWindow::onStopRequested() {
    QString response;
    if (connectionManager->sendEssCommand("ess::stop", response)) {
        terminalOutput->appendPlainText("✓ Stop command sent");
        this->statusBar()->showMessage("Experiment stopped", 2000);
        hideProgress();
    } else {
        terminalOutput->appendPlainText("✗ Failed to send stop command");
        this->statusBar()->showMessage("Failed to stop experiment", 3000);
    }
}

void MainWindow::onResetRequested() {
    QString response;
    if (connectionManager->sendEssCommand("ess::reset", response)) {
        terminalOutput->appendPlainText("✓ Reset command sent");
        this->statusBar()->showMessage("Experiment reset", 2000);
    } else {
        terminalOutput->appendPlainText("✗ Failed to send reset command");
        this->statusBar()->showMessage("Failed to reset experiment", 3000);
    }
}

// Add these new methods for menu actions
void MainWindow::newProject() {
    // TODO: Implement new project dialog
    this->statusBar()->showMessage("New project...", 2000);
}

void MainWindow::openProject() {
    // TODO: Implement open project dialog
    this->statusBar()->showMessage("Open project...", 2000);
}

void MainWindow::saveProject() {
    // TODO: Implement save project
    this->statusBar()->showMessage("Project saved", 2000);
}

void MainWindow::importData() {
    // TODO: Implement data import
    this->statusBar()->showMessage("Import data...", 2000);
}

void MainWindow::exportData() {
    // TODO: Implement data export
    this->statusBar()->showMessage("Export data...", 2000);
}

void MainWindow::showPreferences() {
    // TODO: Implement preferences dialog
    this->statusBar()->showMessage("Preferences...", 2000);
}

void MainWindow::showConnectDialog() {
    // Show the host discovery dock
    dockManager->showDock(DockType::HostDiscovery);
    this->statusBar()->showMessage("Use Connections panel to connect to a host", 3000);
}

void MainWindow::showConnectionSettings() {
    // TODO: Implement connection settings dialog
    this->statusBar()->showMessage("Connection settings...", 2000);
}

void MainWindow::showSystemSelector() {
    // Show the ESS control dock
    dockManager->showDock(DockType::EssControl);
    this->statusBar()->showMessage("Use ESS Control panel to select system", 3000);
}

void MainWindow::showUserGuide() {
    // TODO: Open user guide in browser or help system
    this->statusBar()->showMessage("Opening user guide...", 2000);
}

void MainWindow::showKeyboardShortcuts() {
    QMessageBox::information(this, "Keyboard Shortcuts",
        "Connection:\n"
        "Ctrl+Shift+C - Connect to Host\n"
        "Ctrl+Shift+D - Disconnect\n\n"
        "Experiment:\n"
        "F5 - Start Experiment\n"
        "F6 - Stop Experiment\n"
        "F7 - Reset Experiment\n"
        "F9 - Reload System\n\n"
        "Tools:\n"
        "Ctrl+D - Data Viewer\n"
        "Ctrl+T - Show Terminal\n"
        "Ctrl+E - Show Code Editor\n"
        "Ctrl+Shift+T - Tcl Console\n\n"
        "View:\n"
        "F11 - Full Screen\n"
        "Ctrl+N - New Project\n"
        "Ctrl+O - Open Project\n"
        "Ctrl+S - Save Project");
}

void MainWindow::showAbout() {
    QMessageBox::about(this, "About EssQt",
        "<h3>EssQt</h3>"
        "<p>Experimental Control System Frontend</p>"
        "<p>Version 1.0</p>"
        "<p>Built with Qt " QT_VERSION_STR "</p>"
        "<p>A modern interface for controlling and monitoring "
        "scientific experiments through the ESS system.</p>");
}

void MainWindow::showProgress(const QString& message, int value) {
    this->statusBar()->showMessage(message);
    progressBar->setValue(value);
    progressBar->setVisible(true);
}

void MainWindow::hideProgress() {
    progressBar->setVisible(false);
    this->statusBar()->clearMessage();
}

void MainWindow::reloadSystem() {
    setCursor(Qt::WaitCursor);
    QString response;
    if (connectionManager->sendEssCommand("ess::reload_system", response)) {
        terminalOutput->appendPlainText("✓ System reloaded");
    } else {
        terminalOutput->appendPlainText("✗ Failed to reload system");
    }
    setCursor(Qt::ArrowCursor);
}

void MainWindow::reloadProtocol() {
    setCursor(Qt::WaitCursor);
    QString response;
    if (connectionManager->sendEssCommand("ess::reload_protocol", response)) {
        terminalOutput->appendPlainText("✓ Protocol reloaded");
    } else {
        terminalOutput->appendPlainText("✗ Failed to reload protocol");
    }
    setCursor(Qt::ArrowCursor);
}

void MainWindow::reloadVariant() {
    setCursor(Qt::WaitCursor);
    QString response;
    if (connectionManager->sendEssCommand("ess::reload_variant", response)) {
        terminalOutput->appendPlainText("✓ Variant reloaded");
    } else {
        terminalOutput->appendPlainText("✗ Failed to reload variant");
    }
    setCursor(Qt::ArrowCursor);
}

void MainWindow::saveSettings() {
    QString response;
    if (connectionManager->sendEssCommand("ess::save_settings", response)) {
        terminalOutput->appendPlainText("✓ Settings saved");
    } else {
        terminalOutput->appendPlainText("✗ Failed to save settings");
    }
}

void MainWindow::resetSettings() {
    QString response;
    if (connectionManager->sendEssCommand("ess::reset_settings", response)) {
        terminalOutput->appendPlainText("✓ Settings reset");
        // Automatically reload variant after reset
        reloadVariant();
    } else {
        terminalOutput->appendPlainText("✗ Failed to reset settings");
    }
}

// Terminal Command Handling (keep existing functionality)
// Update your sendCommand method to handle Tcl commands and data loading
void MainWindow::sendCommand() {
    QString line = commandInput->text().trimmed();
    if (line.isEmpty()) return;

    terminalOutput->appendPlainText("> " + line);
    
    // Check if it's a data loading command
    if (line.startsWith("load ")) {
        QString filename = line.mid(5).trimmed();
        if (!filename.isEmpty()) {
            loadDataFile(filename);
        }
    }
    // Check if it's a Tcl command
    else if (line.startsWith("tcl ")) {
        QString tclCmd = line.mid(4);
        onTclCommandRequested(tclCmd);
    }
    // Check if it's a connection command
    else if (line.startsWith("connect ")) {
        QString host = line.mid(8).trimmed();
        if (!host.isEmpty()) {
            connectToHost(host);
        }
    }
    else if (line == "disconnect") {
        disconnectFromHost();
    }
    else if (line.startsWith("ess ") && connectionManager->isConnected()) {
        // Send ESS command directly
        QString essCmd = line.mid(4);
        QString response;
        if (connectionManager->sendEssCommand(essCmd, response)) {
            if (!response.isEmpty()) {
                terminalOutput->appendPlainText(response);
            }
        }
    }
    else if (line.startsWith("dserv ") && connectionManager->isConnected()) {
        // Send dserv command directly
        QString dservCmd = line.mid(6);
        QString response;
        if (connectionManager->sendDservCommand(dservCmd, response)) {
            if (!response.isEmpty()) {
                terminalOutput->appendPlainText(response);
            }
        }
    }
    else {
        // Send to terminal client (existing functionality)
        client->sendMessage(line);
    }
    
    commandInput->clear();
}

void MainWindow::handleResponse(const QString &response) {
    terminalOutput->appendPlainText(response);
}

void MainWindow::handleError(const QString &error) {
    QMessageBox::warning(this, "Terminal Error", error);
}

void MainWindow::onDockVisibilityChanged(DockType type, bool visible) {
    // Handle dock visibility changes if needed
    // qDebug() << "Dock" << static_cast<int>(type) << (visible ? "shown" : "hidden");
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Properly disconnect before closing
    if (connectionManager && connectionManager->isConnected()) {
        connectionManager->disconnectFromHost();
        
        // Give a moment for cleanup to complete
        QEventLoop loop;
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
    }
    
    // Disconnect terminal client if needed
    if (client) {
        client->disconnect();
    }
    
    // Cleanup Tcl interpreter
    if (tclInterpreter) {
        delete tclInterpreter;
        tclInterpreter = nullptr;
    }
    
    event->accept();
}

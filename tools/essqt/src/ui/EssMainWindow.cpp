#include "EssMainWindow.h"
#include "EssWorkspaceManager.h"
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
#include <QDebug>

EssMainWindow::EssMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_workspace(nullptr)
{
    setWindowTitle(QString("EssQt - ESS Control System"));
    
    // No central widget - we use docks for everything
    setCentralWidget(nullptr);    
    
    // Create workspace manager FIRST
    m_workspace = new EssWorkspaceManager(this, this);
    m_workspace->setupWorkspace();
    
    // Now create menus (which need workspace to exist)
    createActions();
    createMenus();
    createStatusBar();
    
    // Connect workspace signals
    connect(m_workspace, &EssWorkspaceManager::statusMessage,
            this, &EssMainWindow::updateStatus);
    
    // Connect to command interface
    connectCommandInterface();
    
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
    
    // View menu - populated by workspace manager
    m_viewMenu = menuBar()->addMenu(tr("&View"));
    QList<QAction*> viewActions = m_workspace->viewMenuActions();
    for (QAction *action : viewActions) {
        m_viewMenu->addAction(action);
    }
    
    // Tools menu
    m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
    // TODO: Add tools
    
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

void EssMainWindow::connectCommandInterface()
{
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    
    connect(cmdInterface, &EssCommandInterface::connected,
            this, &EssMainWindow::onConnected);
    connect(cmdInterface, &EssCommandInterface::disconnected,
            this, &EssMainWindow::onDisconnected);
    connect(cmdInterface, &EssCommandInterface::connectionError,
            this, &EssMainWindow::onConnectionError);
    
    // Connect to data processor for system status updates
    EssDataProcessor *dataProc = EssApplication::instance()->dataProcessor();
    connect(dataProc, &EssDataProcessor::systemStatusUpdated,
            this, [this](const QString &status) {
                updateStatus(QString("System: %1").arg(status), 5000);
            });
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
    
    // Workspace manager handles dock state restoration
}

void EssMainWindow::writeSettings()
{
    EssConfig *config = EssApplication::instance()->config();
    config->setWindowGeometry(saveGeometry());
    
    // Let workspace save its layout
    m_workspace->saveLayout();
    
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

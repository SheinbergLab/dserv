#include "EssCGraphWidget.h"
#include "qtcgwin.hpp"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QComboBox>
#include <QPushButton>

EssCGraphWidget::EssCGraphWidget(QWidget *parent)
    : QWidget(parent)
    , m_commandInterface(nullptr)
    , m_tabWidget(nullptr)
{
    setupUi();
}

EssCGraphWidget::~EssCGraphWidget() = default;

void EssCGraphWidget::setCommandInterface(EssCommandInterface *commandInterface)
{
    m_commandInterface = commandInterface;
    
    if (m_commandInterface && m_commandInterface->tclInterp()) {
        // Create the tab widget with the Tcl interpreter
        m_tabWidget = new QtCGTabWidget(m_commandInterface->tclInterp(), this);
        
        // Add it to our layout
        QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(this->layout());
        if (layout) {
            layout->addWidget(m_tabWidget, 1);
        }
        
        // Connect signals
        connect(m_tabWidget, &QtCGTabWidget::cgraphUpdated,
                this, &EssCGraphWidget::onGraphUpdated);
        
        // Register this widget with Tcl for command access
        registerWithTcl();
    }
}

void EssCGraphWidget::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Create toolbar
    QToolBar *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));
    
    // Add tab action
    QAction *addTabAction = new QAction(QIcon::fromTheme("tab-new"), tr("New Graph"), this);
    addTabAction->setToolTip(tr("Create a new graph tab"));
    connect(addTabAction, &QAction::triggered, this, &EssCGraphWidget::onAddTab);
    toolbar->addAction(addTabAction);
    
    // Export action
    QAction *exportAction = new QAction(QIcon::fromTheme("document-save"), tr("Export"), this);
    exportAction->setToolTip(tr("Export current graph"));
    connect(exportAction, &QAction::triggered, this, &EssCGraphWidget::onExportGraph);
    toolbar->addAction(exportAction);
    
    toolbar->addSeparator();
    
    // Clear action
    QAction *clearAction = new QAction(QIcon::fromTheme("edit-clear"), tr("Clear"), this);
    clearAction->setToolTip(tr("Clear current graph"));
    connect(clearAction, &QAction::triggered, this, &EssCGraphWidget::onClearGraph);
    toolbar->addAction(clearAction);
    
    // Refresh action
    QAction *refreshAction = new QAction(QIcon::fromTheme("view-refresh"), tr("Refresh"), this);
    refreshAction->setToolTip(tr("Refresh current graph"));
    connect(refreshAction, &QAction::triggered, this, &EssCGraphWidget::onRefreshGraph);
    toolbar->addAction(refreshAction);
    
    layout->addWidget(toolbar);
    
    // Tab widget will be added when setCommandInterface is called
}

void EssCGraphWidget::registerWithTcl()
{
    if (!m_commandInterface || !m_tabWidget) return;
    
    Tcl_Interp *interp = m_commandInterface->tclInterp();
    
    // Register the tab widget with Tcl
    QString widgetId = QString("essqt_cgtabs");
    Tcl_SetAssocData(interp, widgetId.toUtf8().constData(), NULL, m_tabWidget);
    
    // Create convenience Tcl procedures
    QString tclScript = R"(
        # Convenience procedures for essqt cgraph
        proc cgraph_add {{label ""}} {
            qtCgAddTab essqt_cgtabs $label
        }
        
        proc cgraph_select {name} {
            qtCgSelectTab essqt_cgtabs $name
        }
        
        proc cgraph_delete {name} {
            qtCgDeleteTab essqt_cgtabs $name
        }
        
        # Override plot command to use current graph
        proc plot {args} {
            # Ensure we have a graph tab
            if {[qtCgGetCurrent] eq ""} {
                cgraph_add "Plot"
            }
            # Call the original plot command
            uplevel 1 plot $args
        }
    )";
    
    Tcl_Eval(interp, tclScript.toUtf8().constData());
}

void EssCGraphWidget::onAddTab()
{
    if (m_tabWidget) {
        QString tabName = m_tabWidget->addCGTab();
        EssConsoleManager::instance()->logInfo(
            QString("Created graph tab: %1").arg(tabName), "CGraph");
    }
}

void EssCGraphWidget::onExportGraph()
{
    QtCGWin* current = QtCGTabManager::getInstance().getCurrentCGWin();
    if (!current) {
        EssConsoleManager::instance()->logWarning("No active graph to export", "CGraph");
        return;
    }
    
    // TODO: Implement export functionality
    // Options: PDF, PNG, SVG, or raw cgraph commands
    EssConsoleManager::instance()->logInfo("Export not yet implemented", "CGraph");
}

void EssCGraphWidget::onClearGraph()
{
    QtCGWin* current = QtCGTabManager::getInstance().getCurrentCGWin();
    if (!current) return;
    
    // Clear via Tcl command
    if (m_commandInterface && m_commandInterface->tclInterp()) {
        QString cmd = QString("qtcgwin_clear %1").arg((quintptr)current);
        Tcl_Eval(m_commandInterface->tclInterp(), cmd.toUtf8().constData());
        current->refresh();
    }
    
    EssConsoleManager::instance()->logInfo("Graph cleared", "CGraph");
}

void EssCGraphWidget::onRefreshGraph()
{
    QtCGWin* current = QtCGTabManager::getInstance().getCurrentCGWin();
    if (current) {
        current->refresh();
    }
}

void EssCGraphWidget::onGraphUpdated()
{
    // Could emit signals or update status here
    emit graphUpdated();
}

// In EssWorkspaceManager.cpp, add this to createDocks():
/*
    // Create CGraph dock
    QDockWidget *cgraphDock = new QDockWidget(tr("Graphs"), m_mainWindow);
    cgraphDock->setObjectName("CGraphDock");
    m_cgraphWidget = new EssCGraphWidget();
    
    // Set the command interface
    auto commandInterface = EssApplication::instance()->commandInterface();
    m_cgraphWidget->setCommandInterface(commandInterface);
    
    cgraphDock->setWidget(m_cgraphWidget);
    m_docks["CGraph"] = cgraphDock;
*/
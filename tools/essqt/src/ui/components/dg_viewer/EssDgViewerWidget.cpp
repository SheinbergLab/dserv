// EssDgViewerWidget.cpp
#include "EssDgViewerWidget.h"
#include "EssDynGroupViewer.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include <QVBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QLabel>

extern "C" {
#include "dlfuncs.h"
}

EssDgViewerWidget::EssDgViewerWidget(QWidget* parent)
    : QWidget(parent)
    , m_tabCounter(0)
    , m_hasPlaceholder(true)
{
    setupUi();
    setupToolbar();
    addPlaceholderTab();
    updateToolbarState();
}

EssDgViewerWidget::~EssDgViewerWidget() = default;

void EssDgViewerWidget::setupUi()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    
    // Toolbar
    m_toolbar = new QToolBar("DG Viewer Tools");
    layout->addWidget(m_toolbar);
    
    // Tab widget with close buttons
    m_tabWidget = new QTabWidget();
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, 
            this, &EssDgViewerWidget::onTabCloseRequested);
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, &EssDgViewerWidget::onCurrentTabChanged);
    
    layout->addWidget(m_tabWidget);
}

void EssDgViewerWidget::setupToolbar()
{
    m_toolbar->setIconSize(QSize(16, 16));
    
    // Show DG actions
    m_showDgAction = m_toolbar->addAction(QIcon::fromTheme("document-open"), "Show DG");
    m_showDgAction->setToolTip("Show a dynamic group (provide DYN_GROUP pointer)");
    connect(m_showDgAction, &QAction::triggered, this, &EssDgViewerWidget::onShowDgRequested);
    
    m_showDgFromTclAction = m_toolbar->addAction(QIcon::fromTheme("view-list-tree"), "From Tcl");
    m_showDgFromTclAction->setToolTip("Load DG from main Tcl interpreter by name");
    connect(m_showDgFromTclAction, &QAction::triggered, this, &EssDgViewerWidget::onShowDgFromTclRequested);
    
    m_toolbar->addSeparator();
    
    // Tab management
    m_closeCurrentAction = m_toolbar->addAction(QIcon::fromTheme("tab-close"), "Close");
    m_closeCurrentAction->setToolTip("Close current tab");
    connect(m_closeCurrentAction, &QAction::triggered, this, &EssDgViewerWidget::onCloseCurrentRequested);
    
    m_closeAllAction = m_toolbar->addAction(QIcon::fromTheme("edit-clear-all"), "Close All");
    m_closeAllAction->setToolTip("Close all tabs");
    connect(m_closeAllAction, &QAction::triggered, this, &EssDgViewerWidget::onCloseAllRequested);
    
    m_toolbar->addSeparator();
    
    // Export
    m_exportCurrentAction = m_toolbar->addAction(QIcon::fromTheme("document-save"), "Export");
    m_exportCurrentAction->setToolTip("Export current DG to CSV");
    connect(m_exportCurrentAction, &QAction::triggered, this, &EssDgViewerWidget::onExportCurrentRequested);
}

void EssDgViewerWidget::showDynGroup(DYN_GROUP* dg, const QString& name)
{
    if (!dg) {
        EssConsoleManager::instance()->logWarning("Cannot show null DYN_GROUP", "DG Viewer");
        return;
    }
    
    // Remove placeholder if it exists
    if (m_hasPlaceholder) {
        removePlaceholderTab();
    }
    
    // Create viewer
    EssDynGroupViewer* viewer = new EssDynGroupViewer();
    viewer->setDynGroup(dg, name);
    
    // Create tab title with basic info
    QString tabTitle = name.isEmpty() ? QString("DG %1").arg(++m_tabCounter) : name;
    
    // Add row/column count if available
    if (viewer->tableWidget() && viewer->tableWidget()->rowCount() > 0) {
        tabTitle += QString(" (%1×%2)")
            .arg(viewer->tableWidget()->rowCount())
            .arg(viewer->tableWidget()->columnCount());
    }
    
    // Add to tab widget
    int tabIndex = m_tabWidget->addTab(viewer, tabTitle);
    m_tabWidget->setCurrentIndex(tabIndex);
    
    // Set tooltip with more details
    m_tabWidget->setTabToolTip(tabIndex, QString("DG: %1\nLists: %2")
        .arg(name.isEmpty() ? "Unnamed" : name)
        .arg(DYN_GROUP_N(dg)));
    
    updateToolbarState();
    emit tabCountChanged(m_tabWidget->count());
    
    EssConsoleManager::instance()->logInfo(
        QString("Showing DG '%1' with %2 lists").arg(name).arg(DYN_GROUP_N(dg)),
        "DG Viewer"
    );
}

void EssDgViewerWidget::showDynGroupFromTcl(const QString& dgName)
{
    auto* app = EssApplication::instance();
    if (!app) {
        EssConsoleManager::instance()->logError("Application not available", "DG Viewer");
        return;
    }
    
    auto* cmdInterface = app->commandInterface();
    if (!cmdInterface) {
        EssConsoleManager::instance()->logError("Command interface not available", "DG Viewer");
        return;
    }
    
    Tcl_Interp* interp = cmdInterface->tclInterp();
    if (!interp) {
        EssConsoleManager::instance()->logError("Tcl interpreter not available", "DG Viewer");
        return;
    }
    
    DYN_GROUP* dg = nullptr;
    int result = tclFindDynGroup(interp, const_cast<char*>(dgName.toUtf8().constData()), &dg);
    
    if (result == TCL_OK && dg) {
        showDynGroup(dg, dgName);
    } else {
        EssConsoleManager::instance()->logWarning(
            QString("Could not find DG '%1' in Tcl interpreter").arg(dgName),
            "DG Viewer"
        );
        
        QMessageBox::warning(this, "DG Not Found", 
            QString("Dynamic group '%1' not found in the main Tcl interpreter.\n\n"
                    "Available DGs: %2").arg(dgName).arg(getAvailableDgNames().join(", ")));
    }
}

void EssDgViewerWidget::closeCurrentTab()
{
    int currentIndex = m_tabWidget->currentIndex();
    if (currentIndex >= 0 && !m_hasPlaceholder) {
        onTabCloseRequested(currentIndex);
    }
}

void EssDgViewerWidget::closeAllTabs()
{
    // Close all real tabs
    while (m_tabWidget->count() > 0) {
        QWidget* widget = m_tabWidget->widget(0);
        m_tabWidget->removeTab(0);
        if (auto* viewer = qobject_cast<EssDynGroupViewer*>(widget)) {
            viewer->deleteLater();
        } else {
            widget->deleteLater();
        }
    }
    
    // Add placeholder
    addPlaceholderTab();
    updateToolbarState();
    emit tabCountChanged(0);
    
    EssConsoleManager::instance()->logInfo("Closed all DG viewer tabs", "DG Viewer");
}

void EssDgViewerWidget::onTabCloseRequested(int index)
{
    QWidget* widget = m_tabWidget->widget(index);
    if (!widget) return;
    
    m_tabWidget->removeTab(index);
    widget->deleteLater();
    
    // Add placeholder if no tabs left
    if (m_tabWidget->count() == 0) {
        addPlaceholderTab();
    }
    
    updateToolbarState();
    emit tabCountChanged(m_hasPlaceholder ? 0 : m_tabWidget->count());
}

void EssDgViewerWidget::onCurrentTabChanged(int index)
{
    updateToolbarState();
}

void EssDgViewerWidget::onShowDgRequested()
{
    // This would typically be called from Tcl with a DYN_GROUP pointer
    // For now, just show the "From Tcl" dialog
    QMessageBox::information(this, "Show DG", 
        "This function is typically called from Tcl with a DYN_GROUP pointer.\n"
        "Use 'From Tcl' to load a DG by name from the interpreter.");
}

void EssDgViewerWidget::onShowDgFromTclRequested()
{
    QStringList availableDgs = getAvailableDgNames();
    
    if (availableDgs.isEmpty()) {
        QMessageBox::information(this, "No DGs Available", 
            "No dynamic groups found in the main Tcl interpreter.");
        return;
    }
    
    bool ok;
    QString dgName = QInputDialog::getItem(this, "Select Dynamic Group",
        "Choose a DG to view:", availableDgs, 0, false, &ok);
    
    if (ok && !dgName.isEmpty()) {
        showDynGroupFromTcl(dgName);
    }
}

void EssDgViewerWidget::onCloseCurrentRequested()
{
    closeCurrentTab();
}

void EssDgViewerWidget::onCloseAllRequested()
{
    if (m_hasPlaceholder) return;
    
    QMessageBox::StandardButton reply = QMessageBox::question(this,
        "Close All Tabs", "Close all DG viewer tabs?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        
    if (reply == QMessageBox::Yes) {
        closeAllTabs();
    }
}

void EssDgViewerWidget::onExportCurrentRequested()
{
    QWidget* currentWidget = m_tabWidget->currentWidget();
    auto* viewer = qobject_cast<EssDynGroupViewer*>(currentWidget);
    
    if (!viewer) {
        QMessageBox::information(this, "Export", "No DG viewer to export.");
        return;
    }
    
    // The EssDynGroupViewer already has export functionality
    // We could trigger it directly or provide a custom export here
    QMessageBox::information(this, "Export", 
        "Use the export button in the viewer's toolbar to export to CSV.");
}

void EssDgViewerWidget::updateToolbarState()
{
    bool hasRealTabs = !m_hasPlaceholder && m_tabWidget->count() > 0;
    bool hasCurrentTab = hasRealTabs && m_tabWidget->currentWidget() != nullptr;
    
    m_closeCurrentAction->setEnabled(hasCurrentTab);
    m_closeAllAction->setEnabled(hasRealTabs);
    m_exportCurrentAction->setEnabled(hasCurrentTab);
}

void EssDgViewerWidget::addPlaceholderTab()
{
    if (m_hasPlaceholder) return;
    
    QWidget* placeholder = new QWidget();
    placeholder->setStyleSheet("QWidget { color: #666; }");
    
    QVBoxLayout* layout = new QVBoxLayout(placeholder);
    layout->addStretch();
    
    QLabel* label = new QLabel("No dynamic groups loaded\n\nUse toolbar to load DGs from Tcl");
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("font-size: 14px; color: #666;");
    layout->addWidget(label);
    
    layout->addStretch();
    
    m_tabWidget->addTab(placeholder, "DG Viewer");
    m_tabWidget->setTabsClosable(false);
    m_hasPlaceholder = true;
}

void EssDgViewerWidget::removePlaceholderTab()
{
    if (!m_hasPlaceholder) return;
    
    if (m_tabWidget->count() > 0) {
        QWidget* placeholder = m_tabWidget->widget(0);
        m_tabWidget->removeTab(0);
        placeholder->deleteLater();
    }
    
    m_tabWidget->setTabsClosable(true);
    m_hasPlaceholder = false;
}

QStringList EssDgViewerWidget::getAvailableDgNames() const
{
    QStringList names;
    
    auto* app = EssApplication::instance();
    if (!app) return names;
    
    auto* cmdInterface = app->commandInterface();
    if (!cmdInterface) return names;
    
    Tcl_Interp* interp = cmdInterface->tclInterp();
    if (!interp) return names;
    
    // Use Tcl to get list of available DGs
    int result = Tcl_Eval(interp, "dg_list");
    if (result == TCL_OK) {
        QString dgList = QString::fromUtf8(Tcl_GetStringResult(interp));
        names = dgList.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    }
    
    return names;
}

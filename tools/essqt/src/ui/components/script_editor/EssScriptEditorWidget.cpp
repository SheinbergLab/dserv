#include <QTabWidget>
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QComboBox>

#include "EssScriptEditorWidget.h"
#include "EssCodeEditor.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "core/EssDataProcessor.h"
#include "console/EssOutputConsole.h"

EssScriptEditorWidget::EssScriptEditorWidget(QWidget *parent)
    : QWidget(parent)
    , m_tabWidget(new QTabWidget(this))
    , m_globalToolbar(new QToolBar(this))
    , m_statusLabel(new QLabel(this))
    , m_pendingSaves(0)
    , m_isGitBusy(false)
{
    setupUi();
    connectSignals();

    // Connect to data processor for script and git datapoints
    if (auto *dataProc = EssApplication::instance()->dataProcessor()) {
      connect(dataProc, &EssDataProcessor::genericDatapointReceived,
	      this, [this](const QString &name, const QVariant &value, qint64) {
		if (name.startsWith("ess/") && name.endsWith("_script")) {
		  onDatapointReceived(name, value.toString());
		} else if (name == "ess/git/branch") {
		  m_currentBranch = value.toString();
		  m_branchCombo->setCurrentText(m_currentBranch);
		} else if (name == "ess/git/branches") {
		  QString branchesStr = value.toString();
		  m_availableBranches = branchesStr.split(' ', Qt::SkipEmptyParts);
                  
		  m_branchCombo->blockSignals(true);
		  m_branchCombo->clear();
		  m_branchCombo->addItems(m_availableBranches);
		  m_branchCombo->setCurrentText(m_currentBranch);
		  m_branchCombo->blockSignals(false);
		}
	      });
    }
    
    // Connect to command interface for connection/disconnection handling
    if (auto *cmdInterface = EssApplication::instance()->commandInterface()) {
      connect(cmdInterface, &EssCommandInterface::connected,
	      this, &EssScriptEditorWidget::updateGitStatus);
      
      // Connect to disconnected signal
      connect(cmdInterface, &EssCommandInterface::disconnected,
              this, &EssScriptEditorWidget::onDisconnected);
    }    
}

EssScriptEditorWidget::~EssScriptEditorWidget() = default;

void EssScriptEditorWidget::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Create global toolbar
    createGlobalToolbar();
    
    // Setup tab widget
    m_tabWidget->setTabPosition(QTabWidget::North);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    
    // Create tabs for each script type
    createScriptTab(SystemScript, "System", "ess/system_script");
    createScriptTab(ProtocolScript, "Protocol", "ess/protocol_script");
    createScriptTab(LoadersScript, "Loaders", "ess/loaders_script");
    createScriptTab(VariantsScript, "Variants", "ess/variants_script");
    createScriptTab(StimScript, "Stim", "ess/stim_script");
    
    // Status bar
    m_statusLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_statusLabel->setMinimumHeight(20);
    
    // Add to layout
    layout->addWidget(m_globalToolbar);
    layout->addWidget(m_tabWidget, 1);  // Give tab widget stretch priority
    layout->addWidget(m_statusLabel);
    
    // Connect tab change signal
    connect(m_tabWidget, &QTabWidget::currentChanged, 
            this, &EssScriptEditorWidget::onTabChanged);
    
    // Set initial state - not connected
    m_statusLabel->setText("Not connected - no scripts loaded");
    
    // Disable all actions initially
    m_saveAction->setEnabled(false);
    m_saveAllAction->setEnabled(false);
    m_reloadAction->setEnabled(false);
    m_pushAction->setEnabled(false);
    m_pullAction->setEnabled(false);
    m_branchCombo->setEnabled(false);
    
    // Update initial state
    updateStatusBar();
    updateGlobalActions();
}

// Update createGlobalToolbar() in EssScriptEditorWidget.cpp:

void EssScriptEditorWidget::createGlobalToolbar()
{
    m_globalToolbar->setMovable(false);
    m_globalToolbar->setIconSize(QSize(16, 16));
    
    // Save current script - with both icon and text for clarity
    m_saveAction = new QAction(tr("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setToolTip(tr("Save current script (Ctrl+S)"));
    // Try theme icon first, but set text as fallback
    QIcon saveIcon = QIcon::fromTheme("document-save");
    if (!saveIcon.isNull()) {
        m_saveAction->setIcon(saveIcon);
    }
    m_saveAction->setEnabled(false);
    connect(m_saveAction, &QAction::triggered, this, &EssScriptEditorWidget::saveCurrentScript);
    
    // Save all scripts - with both icon and text
    m_saveAllAction = new QAction(tr("Save All"), this);
    m_saveAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    m_saveAllAction->setToolTip(tr("Save all modified scripts (Ctrl+Shift+S)"));
    QIcon saveAllIcon = QIcon::fromTheme("document-save-all");
    if (!saveAllIcon.isNull()) {
        m_saveAllAction->setIcon(saveAllIcon);
    }
    m_saveAllAction->setEnabled(false);
    connect(m_saveAllAction, &QAction::triggered, this, &EssScriptEditorWidget::saveAllScripts);
    
    // Reload current script
    m_reloadAction = new QAction(tr("Reload"), this);
    m_reloadAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_reloadAction->setToolTip(tr("Reload current script from server (F5)"));
    QIcon reloadIcon = QIcon::fromTheme("view-refresh");
    if (!reloadIcon.isNull()) {
        m_reloadAction->setIcon(reloadIcon);
    }
    connect(m_reloadAction, &QAction::triggered, this, &EssScriptEditorWidget::reloadCurrentScript);
    
    // Git pull
    m_pullAction = new QAction(tr("Pull"), this);
    m_pullAction->setToolTip(tr("Pull changes from remote repository"));
    QIcon pullIcon = QIcon::fromTheme("go-down");
    if (!pullIcon.isNull()) {
        m_pullAction->setIcon(pullIcon);
    }
    connect(m_pullAction, &QAction::triggered, this, &EssScriptEditorWidget::onPullClicked);
    
    // Git push
    m_pushAction = new QAction(tr("Push"), this);
    m_pushAction->setToolTip(tr("Commit and push changes to remote repository"));
    QIcon pushIcon = QIcon::fromTheme("go-up");
    if (!pushIcon.isNull()) {
        m_pushAction->setIcon(pushIcon);
    }
    connect(m_pushAction, &QAction::triggered, this, &EssScriptEditorWidget::onPushClicked);
    
    // Branch selector
    m_branchCombo = new QComboBox(this);
    m_branchCombo->setMinimumWidth(120);
    m_branchCombo->setMaximumWidth(200);
    m_branchCombo->setToolTip(tr("Current Git branch"));
    connect(m_branchCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssScriptEditorWidget::onBranchChanged);
    
    // Add all to toolbar with proper grouping
    m_globalToolbar->addAction(m_saveAction);
    m_globalToolbar->addAction(m_saveAllAction);
    m_globalToolbar->addSeparator();
    m_globalToolbar->addAction(m_reloadAction);
    m_globalToolbar->addSeparator();
    m_globalToolbar->addAction(m_pullAction);
    m_globalToolbar->addAction(m_pushAction);
    m_globalToolbar->addWidget(new QLabel(" Branch: "));
    m_globalToolbar->addWidget(m_branchCombo);
    
    // Set toolbar button style to show both icon and text
    m_globalToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // m_globalToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // m_globalToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
}

void EssScriptEditorWidget::createScriptTab(ScriptType type, const QString &tabName, 
                                           const QString &datapointName)
{
    auto *editor = new EssCodeEditor(this);
    editor->setLanguage(EssCodeEditor::Tcl);
    editor->setToolbarVisible(false);
    
    ScriptEditor scriptEditor;
    scriptEditor.editor = editor;
    scriptEditor.type = type;
    scriptEditor.datapointName = datapointName;
    scriptEditor.loaded = false;
    
    m_scriptEditors[type] = scriptEditor;
    
    // Add to tab widget
    m_tabWidget->addTab(editor, tabName);
    
    // Simple connection to modificationChanged signal only
    connect(editor, &EssCodeEditor::modificationChanged,
            this, [this, type](bool modified) {
                updateTabTitle(type);
                updateGlobalActions();
                emit scriptModified(type, modified);
            });
            
    connect(editor, &EssCodeEditor::saveRequested,
            this, [this, type]() {
                saveScript(type);
            });
            
    connect(editor, &EssCodeEditor::cursorPositionChanged,
            this, &EssScriptEditorWidget::onCursorPositionChanged);
}

void EssScriptEditorWidget::connectSignals()
{
    // Nothing additional needed here - connections are made in createScriptTab
}

void EssScriptEditorWidget::onDatapointReceived(const QString &name, const QString &content)
{
  ScriptType type = scriptTypeFromDatapoint(name);
    
    // Find the corresponding editor
    auto it = m_scriptEditors.find(type);
    if (it != m_scriptEditors.end()) {
        loadScript(type, content);
    }
}

void EssScriptEditorWidget::loadScript(ScriptType type, const QString &content)
{
    auto it = m_scriptEditors.find(type);
    if (it == m_scriptEditors.end()) return;
    
    ScriptEditor &scriptEditor = it.value();
    
    // Only update if content actually changed
    if (scriptEditor.editor->content() != content) {
        scriptEditor.editor->setContent(content);
        scriptEditor.loaded = true;
        
        // Clear modification flag since this is freshly loaded
        scriptEditor.editor->setModified(false);
        
        updateTabTitle(type);
        
        EssConsoleManager::instance()->logInfo(
            QString("Loaded %1 script (%2 bytes)")
                .arg(scriptTypeToString(type))
                .arg(content.length()),
            "ScriptEditor"
        );
    }
}

QString EssScriptEditorWidget::getScriptContent(ScriptType type) const
{
    auto it = m_scriptEditors.find(type);
    if (it != m_scriptEditors.end()) {
        return it->editor->content();
    }
    return QString();
}

bool EssScriptEditorWidget::hasModifiedScripts() const
{
    for (const auto &scriptEditor : m_scriptEditors) {
        if (scriptEditor.editor->isModified()) {
            return true;
        }
    }
    return false;
}

QList<EssScriptEditorWidget::ScriptType> EssScriptEditorWidget::modifiedScripts() const
{
    QList<ScriptType> modified;
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        if (it->editor->isModified()) {
            modified.append(it.key());
        }
    }
    return modified;
}

void EssScriptEditorWidget::saveScript(ScriptType type)
{
    auto it = m_scriptEditors.find(type);
    if (it == m_scriptEditors.end() || !it->editor->isModified()) return;
    
    QString content = it->editor->content();
    QString scriptName = scriptTypeToString(type).toLower();
    
    // Send save command to backend
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        m_pendingSaves++;
        
        QString cmd = QString("::ess::save_script %1 {%2}")
            .arg(scriptName)
            .arg(content);
        
        auto result = cmdInterface->executeEss(cmd);
        
        m_pendingSaves--;
        
        if (result.status == EssCommandInterface::StatusSuccess) {
            // Mark as saved
            it->editor->setModified(false);
            updateTabTitle(type);
            
            emit scriptSaved(type);
            emit statusMessage(tr("%1 script saved").arg(scriptTypeToString(type)), 3000);
            
            EssConsoleManager::instance()->logSuccess(
                QString("%1 script saved").arg(scriptTypeToString(type)),
                "ScriptEditor"
            );
        } else {
            QMessageBox::warning(this, tr("Save Failed"),
                tr("Failed to save %1 script: %2")
                    .arg(scriptTypeToString(type))
                    .arg(result.error));
        }
    } else {
        QMessageBox::warning(this, tr("Not Connected"),
            tr("Cannot save script - not connected to ESS backend"));
    }
}

void EssScriptEditorWidget::saveCurrentScript()
{
    ScriptType type = currentScriptType();
    saveScript(type);
}

void EssScriptEditorWidget::saveAllScripts()
{
    int savedCount = 0;
    QList<ScriptType> toSave = modifiedScripts();
    
    for (ScriptType type : toSave) {
        saveScript(type);
        savedCount++;
    }
    
    if (savedCount > 0) {
        emit statusMessage(tr("Saved %1 script(s)").arg(savedCount), 3000);
        emit saveAllRequested();
    }
}

void EssScriptEditorWidget::reloadCurrentScript()
{
    ScriptType type = currentScriptType();
    auto it = m_scriptEditors.find(type);
    if (it == m_scriptEditors.end()) return;
    
    // Check if script is modified
    if (it->editor->isModified()) {
        auto reply = QMessageBox::question(this, tr("Reload Script"),
            tr("The %1 script has unsaved changes. Reload anyway?")
                .arg(scriptTypeToString(type)),
            QMessageBox::Yes | QMessageBox::No);
        
        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    
    // Request fresh data from backend
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        QString datapointName = it->datapointName;
        cmdInterface->executeDserv(QString("%%touch %1").arg(datapointName));
        
        emit statusMessage(tr("Reloading %1 script...").arg(scriptTypeToString(type)), 2000);
        
        EssConsoleManager::instance()->logInfo(
            QString("Reloading %1 script").arg(scriptTypeToString(type)),
            "ScriptEditor"
        );
    }
}

void EssScriptEditorWidget::clearAllScripts()
{
    // Block signals to prevent unnecessary updates during clearing
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        it->editor->blockSignals(true);
        
        // Clear the content
        it->editor->setContent("");
        
        // Reset the loaded flag
        it->loaded = false;
        
        // Clear any modification flags
        it->editor->setModified(false);
        
        it->editor->blockSignals(false);
    }
    
    // Clear git information
    m_currentBranch.clear();
    m_availableBranches.clear();
    m_branchCombo->clear();
    m_branchCombo->setCurrentIndex(-1);
    
    // Update all tab titles to remove any modification indicators
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        updateTabTitle(it.key());
    }
    
    // Update UI state
    updateStatusBar();
    updateGlobalActions();
    
    // Log the action
    EssConsoleManager::instance()->logInfo(
        "All scripts cleared on disconnect", 
        "ScriptEditor"
    );
}

void EssScriptEditorWidget::onDisconnected()
{
    // Check if we have any unsaved changes
    if (hasModifiedScripts()) {
        // Since we're disconnecting, we can't save anyway, so just warn in the log
        EssConsoleManager::instance()->logWarning(
            "Disconnecting with unsaved script changes - changes will be lost",
            "ScriptEditor"
        );
    }
    
    // Clear all scripts
    clearAllScripts();
    
    // Reset any pending operations
    m_pendingSaves = 0;
    m_isGitBusy = false;
    
    // Update status to show disconnected state
    emit statusMessage(tr("Disconnected - scripts cleared"), 3000);
}

bool EssScriptEditorWidget::confirmDisconnectWithUnsavedChanges()
{
    if (!hasModifiedScripts()) {
        return true;  // No unsaved changes, OK to disconnect
    }
    
    QStringList modifiedList;
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        if (it->editor->isModified()) {
            modifiedList << scriptTypeToString(it.key());
        }
    }
    
    QString message = tr("The following scripts have unsaved changes:\n\n%1\n\n"
                        "These changes will be lost if you disconnect. Continue?")
                        .arg(modifiedList.join(", "));
    
    int result = QMessageBox::warning(this, 
                                     tr("Unsaved Script Changes"),
                                     message,
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No);
    
    return (result == QMessageBox::Yes);
}

void EssScriptEditorWidget::onTabChanged(int index)
{
    Q_UNUSED(index)
    updateStatusBar();
    updateGlobalActions();
}

void EssScriptEditorWidget::onScriptModified(bool modified)
{
    // Find which editor sent the signal
    EssCodeEditor *editor = qobject_cast<EssCodeEditor*>(sender());
    if (!editor) return;
    
    // Find the corresponding script type
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        if (it->editor == editor) {
            updateTabTitle(it.key());
            emit scriptModified(it.key(), modified);
            break;
        }
    }
    
    updateGlobalActions();
}

void EssScriptEditorWidget::onPushClicked()
{
    // Check for unsaved changes
    if (hasModifiedScripts()) {
        auto reply = QMessageBox::question(this, tr("Unsaved Changes"),
            tr("You have unsaved scripts. Save all before pushing?"),
            QMessageBox::Save | QMessageBox::Cancel);
            
        if (reply == QMessageBox::Cancel) return;
        
        saveAllScripts();
    }
    
    m_isGitBusy = true;
    m_pushAction->setEnabled(false);
    m_pullAction->setEnabled(false);
    m_branchCombo->setEnabled(false);
    
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        emit statusMessage(tr("Pushing changes to remote..."), 0);
        
        // Execute git push through backend
        auto result = cmdInterface->executeEss("send git git::commit_and_push");
        
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit statusMessage(tr("Push completed successfully"), 3000);
            EssConsoleManager::instance()->logSuccess("Git push completed", "ScriptEditor");
        } else {
            QMessageBox::warning(this, tr("Push Failed"),
                tr("Failed to push changes: %1").arg(result.error));
        }
    }
    
    m_isGitBusy = false;
    updateGlobalActions();
}

void EssScriptEditorWidget::onPullClicked()
{
    // Warn about unsaved changes
    if (hasModifiedScripts()) {
        auto reply = QMessageBox::warning(this, tr("Unsaved Changes"),
            tr("You have unsaved changes. Pull will overwrite them. Continue?"),
            QMessageBox::Yes | QMessageBox::No);
            
        if (reply != QMessageBox::Yes) return;
    }
    
    m_isGitBusy = true;
    m_pushAction->setEnabled(false);
    m_pullAction->setEnabled(false);
    m_branchCombo->setEnabled(false);
    
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        emit statusMessage(tr("Pulling changes from remote..."), 0);
        
        auto result = cmdInterface->executeEss("send git git::pull");
        
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit statusMessage(tr("Pull completed successfully"), 3000);
            EssConsoleManager::instance()->logSuccess("Git pull completed", "ScriptEditor");
            
            // Reload all scripts after pull
            QTimer::singleShot(500, this, [this]() {
                for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
                    QString datapointName = it->datapointName;
                    auto *cmdInterface = EssApplication::instance()->commandInterface();
                    if (cmdInterface && cmdInterface->isConnected()) {
                        cmdInterface->executeDserv(QString("%%touch %1").arg(datapointName));
                    }
                }
            });
        } else {
            QMessageBox::warning(this, tr("Pull Failed"),
                tr("Failed to pull changes: %1").arg(result.error));
        }
    }
    
    m_isGitBusy = false;
    updateGlobalActions();
}

void EssScriptEditorWidget::onBranchChanged(int index)
{
    if (index < 0 || m_isGitBusy) return;
    
    QString newBranch = m_branchCombo->itemText(index);
    if (newBranch == m_currentBranch) return;
    
    // Warn about unsaved changes
    if (hasModifiedScripts()) {
        auto reply = QMessageBox::warning(this, tr("Unsaved Changes"),
            tr("You have unsaved changes. Switching branches will lose them. Continue?"),
            QMessageBox::Yes | QMessageBox::No);
            
        if (reply != QMessageBox::Yes) {
            // Restore previous selection
            m_branchCombo->setCurrentText(m_currentBranch);
            return;
        }
    }
    
    m_isGitBusy = true;
    updateGlobalActions();
    
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        emit statusMessage(tr("Switching to branch %1...").arg(newBranch), 0);
        
        QString cmd = QString("send git {git::switch_and_pull %1}").arg(newBranch);
        auto result = cmdInterface->executeEss(cmd);
        
        if (result.status == EssCommandInterface::StatusSuccess) {
            m_currentBranch = newBranch;
            emit statusMessage(tr("Switched to branch %1").arg(newBranch), 3000);
            
            // Reload system after branch switch
            cmdInterface->executeEss("ess::reload_variant");
        } else {
            QMessageBox::warning(this, tr("Branch Switch Failed"),
                tr("Failed to switch branch: %1").arg(result.error));
            // Restore combo to current branch
            m_branchCombo->setCurrentText(m_currentBranch);
        }
    }
    
    m_isGitBusy = false;
    updateGlobalActions();
}

void EssScriptEditorWidget::updateGitStatus()
{
    auto *cmdInterface = EssApplication::instance()->commandInterface();
    if (!cmdInterface || !cmdInterface->isConnected()) return;
    
    // Request current branch and available branches
    cmdInterface->executeDserv("%touch ess/git/branch");
    cmdInterface->executeDserv("%touch ess/git/branches");
}

void EssScriptEditorWidget::onEditorSaveRequested()
{
    saveCurrentScript();
}

void EssScriptEditorWidget::onCursorPositionChanged(int line, int column)
{
    updateStatusBar();
}

void EssScriptEditorWidget::updateTabTitle(ScriptType type)
{
    auto it = m_scriptEditors.find(type);
    if (it == m_scriptEditors.end()) return;
    
    QString baseTitle = scriptTypeToString(type);
    
    // Find tab index
    int index = -1;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (m_tabWidget->widget(i) == it->editor) {
            index = i;
            break;
        }
    }
    
    if (index >= 0) {
        QString title = baseTitle;
        
        // Add modified indicator
        if (it->editor->isModified()) {
            // Use bullet character for modified indicator
            title = "● " + title;
            // OR use traditional asterisk
            // title = title + " *";
        }
        
        m_tabWidget->setTabText(index, title);
        
        // Optional: Use tooltip to indicate modified state
        if (it->editor->isModified()) {
            m_tabWidget->setTabToolTip(index, baseTitle + " (modified)");
        } else {
            m_tabWidget->setTabToolTip(index, baseTitle);
        }
    }
}

void EssScriptEditorWidget::updateStatusBar()
{
    ScriptType type = currentScriptType();
    auto it = m_scriptEditors.find(type);
    if (it != m_scriptEditors.end()) {
        int line, column;
        it->editor->getCursorPosition(line, column);
        
        QString status = QString("%1 Script - Line %2, Column %3")
            .arg(scriptTypeToString(type))
            .arg(line + 1)
            .arg(column + 1);
        
        if (it->editor->isModified()) {
            status += " - Modified";
        }
        
        if (!m_currentBranch.isEmpty()) {
            status += QString(" - Branch: %1").arg(m_currentBranch);
        }
        
        m_statusLabel->setText(status);
    }
}

void EssScriptEditorWidget::updateGlobalActions()
{
    ScriptType type = currentScriptType();
    auto it = m_scriptEditors.find(type);
    
    if (it != m_scriptEditors.end()) {
        // Temporarily remove connection check for testing
        bool modified = it->editor->isModified();
        m_saveAction->setEnabled(modified);
    } else {
        m_saveAction->setEnabled(false);
    }
    
    // Keep other buttons with connection check
    bool isConnected = false;
    if (auto *cmdInterface = EssApplication::instance()->commandInterface()) {
        isConnected = cmdInterface->isConnected();
    }
    
    m_saveAllAction->setEnabled(hasModifiedScripts() && isConnected);
    m_reloadAction->setEnabled(isConnected);
    m_pushAction->setEnabled(!m_isGitBusy && isConnected);
    m_pullAction->setEnabled(!m_isGitBusy && isConnected);
    m_branchCombo->setEnabled(!m_isGitBusy && isConnected);
}

QString EssScriptEditorWidget::scriptTypeToString(ScriptType type) const
{
    switch (type) {
        case SystemScript: return "System";
        case ProtocolScript: return "Protocol";
        case LoadersScript: return "Loaders";
        case VariantsScript: return "Variants";
        case StimScript: return "Stim";
        default: return "Unknown";
    }
}

QString EssScriptEditorWidget::scriptTypeToDatapoint(ScriptType type)
{
    switch (type) {
        case SystemScript: return "ess/system_script";
        case ProtocolScript: return "ess/protocol_script";
        case LoadersScript: return "ess/loaders_script";
        case VariantsScript: return "ess/variants_script";
        case StimScript: return "ess/stim_script";
        default: return "";
    }
}

EssScriptEditorWidget::ScriptType EssScriptEditorWidget::scriptTypeFromDatapoint(const QString &name) const
{
    static const QMap<QString, ScriptType> dpMap = {
        {"ess/system_script", SystemScript},
        {"ess/protocol_script", ProtocolScript},
        {"ess/loaders_script", LoadersScript},
        {"ess/variants_script", VariantsScript},
        {"ess/stim_script", StimScript}
    };
    
    return dpMap.value(name, SystemScript);
}

EssScriptEditorWidget::ScriptType EssScriptEditorWidget::currentScriptType() const
{
    QWidget *currentWidget = m_tabWidget->currentWidget();
    
    for (auto it = m_scriptEditors.begin(); it != m_scriptEditors.end(); ++it) {
        if (it->editor == currentWidget) {
            return it.key();
        }
    }
    
    return SystemScript;  // Default
}
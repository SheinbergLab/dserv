// EssScriptableWidget.cpp
#include "EssScriptableWidget.h"
#include "EssCodeEditor.h"
#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "console/EssOutputConsole.h"
#include "scriptable_widget/EssScriptableManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QTextCursor>
#include <QMessageBox>

extern "C" {
#include "dlfuncs.h"
}

EssScriptableWidget::EssScriptableWidget(const QString& name, QWidget* parent)
    : QWidget(parent)
    , m_name(name.isEmpty() ? QString("scriptable_%1").arg(QDateTime::currentMSecsSinceEpoch()) : name)
    , m_interp(nullptr)
    , m_mainInterp(nullptr)
    , m_initialized(false)
    , m_developmentMode(false)
    , m_devLayoutMode(DevHidden)
    , m_functionalWidget(nullptr)
    , m_scriptEditor(nullptr)
    , m_localConsole(nullptr)
    , m_devToolbar(nullptr)
    , m_splitter(nullptr)
    , m_tabWidget(nullptr)
    , m_floatingEditor(nullptr)
    , m_commandLine(nullptr)
    , m_historyIndex(0)
{
    connectToDataProcessor();
}

EssScriptableWidget::~EssScriptableWidget()
{
    if (m_interp) {
        Tcl_DeleteInterp(m_interp);
    }
}

void EssScriptableWidget::initializeWidget()
{
    // Now it's safe to call virtual functions
    initializeInterpreter();
    setupDevelopmentUI();
    
    if (!m_setupScript.isEmpty()) {
        executeSetupScript();
    }
}

void EssScriptableWidget::initializeInterpreter()
{
    // Create interpreter
    m_interp = Tcl_CreateInterp();
    if (!m_interp) {
        localLog("ERROR: Failed to create Tcl interpreter");
        return;
    }
    
    // Initialize Tcl
    if (Tcl_Init(m_interp) != TCL_OK) {
        localLog(QString("ERROR: Failed to initialize Tcl: %1")
                  .arg(Tcl_GetStringResult(m_interp)));
        Tcl_DeleteInterp(m_interp);
        m_interp = nullptr;
        return;
    }
    
    // Store widget pointer in interpreter
    Tcl_SetAssocData(m_interp, "scriptable_widget", nullptr, this);
    
    // Load required packages
    const char* initScript = R"tcl(
        # Load required packages if available
        catch { package require dlsh }
    )tcl";
    
    if (Tcl_Eval(m_interp, initScript) != TCL_OK) {
        localLog(QString("Warning: Package loading failed: %1")
                  .arg(Tcl_GetStringResult(m_interp)));
    }
    
    // Register core commands
    registerCoreCommands();
    
    // NOW it's safe to call the virtual function
    registerCustomCommands();
    
    m_initialized = true;
    emit initialized();
    
    localLog(QString("Scriptable widget '%1' initialized").arg(m_name));
}

void EssScriptableWidget::setupDevelopmentUI()
{
    // Create main layout
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(2, 2, 2, 2);
    m_mainLayout->setSpacing(2);
    
    // Create development toolbar (initially hidden)
    createDevelopmentToolbar();
    m_mainLayout->addWidget(m_devToolbar);
    m_devToolbar->setVisible(false);
    
    // Create main functional widget
    m_functionalWidget = createMainWidget();
    if (m_functionalWidget) {
        m_mainLayout->addWidget(m_functionalWidget);
    }
    
    m_historyIndex = 0;
}

void EssScriptableWidget::createDevelopmentToolbar()
{
    m_devToolbar = new QToolBar("Development", this);
    m_devToolbar->setIconSize(QSize(16, 16));
    m_devToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    
    // Development mode toggle
    m_toggleDevModeAction = m_devToolbar->addAction("Dev");
    m_toggleDevModeAction->setCheckable(true);
    m_toggleDevModeAction->setToolTip("Toggle development mode");
    connect(m_toggleDevModeAction, &QAction::toggled, this, &EssScriptableWidget::onDevelopmentModeToggled);
    
    m_devToolbar->addSeparator();
    
    // Layout mode selector
    m_devToolbar->addWidget(new QLabel("Layout:"));
    m_layoutModeCombo = new QComboBox();
    m_layoutModeCombo->addItem("Hidden", DevHidden);
    m_layoutModeCombo->addItem("Bottom", DevBottomPanel);
    m_layoutModeCombo->addItem("Tabs", DevTabbed);
    connect(m_layoutModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &EssScriptableWidget::onLayoutModeChanged);
    m_devToolbar->addWidget(m_layoutModeCombo);
    
    m_devToolbar->addSeparator();
    
    // Script actions
    m_testScriptAction = m_devToolbar->addAction("Test");
    m_testScriptAction->setToolTip("Execute current script");
    connect(m_testScriptAction, &QAction::triggered, this, &EssScriptableWidget::onTestScript);
    
    m_resetScriptAction = m_devToolbar->addAction("Reset");
    m_resetScriptAction->setToolTip("Reset to default script");
    connect(m_resetScriptAction, &QAction::triggered, this, &EssScriptableWidget::onResetScript);
    
    m_generateCodeAction = m_devToolbar->addAction("C++");
    m_generateCodeAction->setToolTip("Generate C++ code");
    connect(m_generateCodeAction, &QAction::triggered, this, &EssScriptableWidget::onGenerateCode);
}


void EssScriptableWidget::registerCoreCommands()
{
    if (!m_interp) return;
    
    // Core commands available to all scriptable widgets
    Tcl_CreateObjCommand(m_interp, "bind_datapoint", tcl_bind_datapoint, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "shared_dg", tcl_shared_dg, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "local_log", tcl_local_log, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "test_datapoint", tcl_test_datapoint, this, nullptr);
}

void EssScriptableWidget::connectToDataProcessor()
{
    auto* app = EssApplication::instance();
    if (!app) return;
    
    auto* processor = app->dataProcessor();
    if (!processor) return;
    
    // Connect to data updates
    connect(processor, &EssDataProcessor::genericDatapointReceived,
            this, &EssScriptableWidget::onDatapointReceived_internal);
    
    // Connect to specific data types that might be relevant
    connect(processor, &EssDataProcessor::stimulusDataReceived,
            this, [this](const QByteArray&, qint64 timestamp) {
                onDatapointReceived_internal("stimdg", QVariant(), timestamp);
            });
    
    connect(processor, &EssDataProcessor::trialDataReceived,
            this, [this](const QByteArray&, qint64 timestamp) {
                onDatapointReceived_internal("trialdg", QVariant(), timestamp);
            });
}

int EssScriptableWidget::eval(const QString& command)
{
    if (!m_interp) {
        localLog("ERROR: No interpreter available");
        return TCL_ERROR;
    }
    
    int result = Tcl_Eval(m_interp, command.toUtf8().constData());
    QString output = QString::fromUtf8(Tcl_GetStringResult(m_interp));
    
    if (result != TCL_OK) {
        localLog(QString("Tcl Error: %1").arg(output));
    }
    
    emit scriptExecuted(result, output);
    return result;
}

QString EssScriptableWidget::result() const
{
    if (!m_interp) return QString();
    return QString::fromUtf8(Tcl_GetStringResult(m_interp));
}

void EssScriptableWidget::setSetupScript(const QString& script)
{
    m_setupScript = script;
    if (m_setupScript.isEmpty()) {
        m_setupScript = m_defaultSetupScript;
    }
    
    if (m_scriptEditor) {
        m_scriptEditor->setContent(m_setupScript);
    }
}

void EssScriptableWidget::executeSetupScript()
{
    if (m_setupScript.isEmpty() || !m_interp) return;
    
    localLog("=== Executing setup script ===");
    
    // Clear existing bindings
    m_datapointBindings.clear();
    m_eventBindings.clear();
    
    int result = eval(m_setupScript);
    
    if (result == TCL_OK) {
        localLog("Setup script completed successfully");
        onSetupComplete();
    } else {
        localLog("Setup script failed");
    }
}

void EssScriptableWidget::bindDatapoint(const QString& dpointName, const QString& script)
{
    m_datapointBindings[dpointName] = script;
    localLog(QString("Bound datapoint: %1").arg(dpointName));
    emit datapointBound(dpointName, script);
}

void EssScriptableWidget::bindEvent(const QString& eventPattern, const QString& script)
{
    m_eventBindings[eventPattern] = script;
    localLog(QString("Bound event: %1").arg(eventPattern));
}

void EssScriptableWidget::onDatapointReceived_internal(const QString& name, const QVariant& value, qint64 timestamp)
{
    // Check for matching bindings
    for (auto it = m_datapointBindings.begin(); it != m_datapointBindings.end(); ++it) {
        QString pattern = it.key();
        bool matches = false;
        
        if (pattern.contains("*")) {
            // Simple wildcard matching - convert * to empty and check if name contains the pattern
            QString basePattern = QString(pattern).replace("*", "");
            matches = name.contains(basePattern);
        } else {
            // Exact match
            matches = (name == pattern);
        }
        
        if (matches) {
            QString script = substituteDatapointData(it.value(), name, value, timestamp);
            eval(script);
        }
    }
    
    // Call virtual handler
    onDatapointReceived(name, value, timestamp);
}

QString EssScriptableWidget::substituteDatapointData(const QString& script, const QString& name,
                                                     const QVariant& value, qint64 timestamp) const
{
    QString result = script;
    
    // Substitute datapoint variables
    result.replace("$dpoint_name", name);
    result.replace("$dpoint_value", value.toString());
    result.replace("$dpoint_timestamp", QString::number(timestamp));
    
    // Widget info
    result.replace("$widget_name", m_name);
    result.replace("$widget_type", getWidgetTypeName());
    
    return result;
}

void EssScriptableWidget::setDevelopmentMode(bool enabled)
{
    if (m_developmentMode == enabled) return;
    
    m_developmentMode = enabled;
    m_devToolbar->setVisible(enabled);
    
    if (enabled) {
        // Create development components if needed
        if (!m_scriptEditor) {
            createScriptEditor();
        }
        if (!m_localConsole) {
            createLocalConsole();
        }
        
        // Apply current layout
        applyDevelopmentLayout();
        
        localLog("Development mode enabled");
    } else {
        // Hide development UI but keep components for quick re-enable
        setDevelopmentLayout(DevHidden);
        localLog("Development mode disabled");
    }
    
    m_toggleDevModeAction->setChecked(enabled);
}

void EssScriptableWidget::setDevelopmentLayout(DevLayoutMode mode)
{
    if (m_devLayoutMode == mode) return;
    
    // Clean up current layout
    if (m_splitter) {
        // Remove all widgets from splitter before deleting it
        while (m_splitter->count() > 0) {
            QWidget* widget = m_splitter->widget(0);
            widget->setParent(nullptr);  // Unparent the widget
        }
        m_splitter->setVisible(false);
        m_mainLayout->removeWidget(m_splitter);
        m_splitter->deleteLater();
        m_splitter = nullptr;
    }
    
    if (m_tabWidget) {
        // Remove all widgets from tab widget before deleting it
        while (m_tabWidget->count() > 0) {
            QWidget* widget = m_tabWidget->widget(0);
            m_tabWidget->removeTab(0);
            widget->setParent(nullptr);  // Unparent the widget
        }
        m_tabWidget->setVisible(false);
        m_mainLayout->removeWidget(m_tabWidget);
        m_tabWidget->deleteLater();
        m_tabWidget = nullptr;
    }
    
    if (m_floatingEditor) {
        m_floatingEditor->hide();
    }
    
    // Ensure ALL development widgets are unparented and ready for reuse
    if (m_scriptEditor && m_scriptEditor->parent()) {
        m_scriptEditor->setParent(nullptr);
    }
    if (m_localConsole && m_localConsole->parent()) {
        m_localConsole->setParent(nullptr);
    }
    
    // Ensure functional widget is back in main layout
    if (m_functionalWidget && m_functionalWidget->parent() != this) {
        m_functionalWidget->setParent(this);
        m_mainLayout->addWidget(m_functionalWidget);
    }
    
    m_devLayoutMode = mode;
    
    // Update combo box
    if (m_layoutModeCombo) {
        m_layoutModeCombo->setCurrentIndex(m_layoutModeCombo->findData(mode));
    }
    
    // Apply new layout
    applyDevelopmentLayout();
}

void EssScriptableWidget::applyDevelopmentLayout()
{
    switch (m_devLayoutMode) {
        case DevHidden:
            // Nothing to do - just functional widget
            break;
        case DevBottomPanel:
            setupBottomPanelLayout();
            break;
        case DevTabbed:
            setupTabbedLayout();
            break;
    }
}

void EssScriptableWidget::setupBottomPanelLayout()
{
    if (!m_scriptEditor || !m_localConsole) return;
    
    // Create vertical splitter
    m_splitter = new QSplitter(Qt::Vertical, this);
    
    // Remove functional widget from main layout and add to splitter
    m_mainLayout->removeWidget(m_functionalWidget);
    m_functionalWidget->setParent(m_splitter);
    m_splitter->addWidget(m_functionalWidget);
    
    // Create bottom panel with tabs AND command line
    QWidget* bottomPanel = new QWidget(m_splitter);
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(2);
    
    // Create tabbed script editor and console
    QTabWidget* bottomTabs = new QTabWidget(bottomPanel);
    m_scriptEditor->setParent(bottomTabs);
    m_localConsole->setParent(bottomTabs);
    bottomTabs->addTab(m_scriptEditor, "Script");
    bottomTabs->addTab(m_localConsole, "Console");
    
    bottomLayout->addWidget(bottomTabs);
    
    // Add command line at bottom
    QLabel* cmdLabel = new QLabel("Command:", bottomPanel);
    cmdLabel->setStyleSheet("font-weight: bold; color: #666;");
    bottomLayout->addWidget(cmdLabel);
    bottomLayout->addWidget(m_commandLine);
    
    m_splitter->addWidget(bottomPanel);
    
    // Set proportions
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);
    
    m_mainLayout->addWidget(m_splitter);
}

void EssScriptableWidget::setupTabbedLayout()
{
    if (!m_scriptEditor || !m_localConsole) return;
    
    // Create main container for tabs + command line
    QWidget* tabbedContainer = new QWidget(this);
    QVBoxLayout* containerLayout = new QVBoxLayout(tabbedContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(2);
    
    // Create tab widget
    m_tabWidget = new QTabWidget(tabbedContainer);
    
    // Remove functional widget from main layout
    m_mainLayout->removeWidget(m_functionalWidget);
    
    // Ensure all widgets are properly parented to the tab widget
    m_functionalWidget->setParent(m_tabWidget);
    m_scriptEditor->setParent(m_tabWidget);
    m_localConsole->setParent(m_tabWidget);
    
    // Add tabs
    m_tabWidget->addTab(m_functionalWidget, getWidgetTypeName());
    m_tabWidget->addTab(m_scriptEditor, "Script");
    m_tabWidget->addTab(m_localConsole, "Console");
    
    containerLayout->addWidget(m_tabWidget);
    
    // Add command line at bottom
    QLabel* cmdLabel = new QLabel("Command:", tabbedContainer);
    cmdLabel->setStyleSheet("font-weight: bold; color: #666;");
    containerLayout->addWidget(cmdLabel);
    containerLayout->addWidget(m_commandLine);
    
    m_mainLayout->addWidget(tabbedContainer);
}

void EssScriptableWidget::createScriptEditor()
{
    m_scriptEditor = new EssCodeEditor();
    m_scriptEditor->setLanguage(EssCodeEditor::Tcl);
    m_scriptEditor->setContent(m_setupScript.isEmpty() ? 
        "# Component setup script\n# Use 'bind_datapoint' to connect to data\n\nlocal_log \"Component initialized\"\n" : 
        m_setupScript);
    m_scriptEditor->setToolbarVisible(false); // Keep it compact
    
    connect(m_scriptEditor, &EssCodeEditor::contentChanged, 
            [this](const QString& content) {
                m_setupScript = content;
            });
    if (!m_commandLine) {
        createCommandLine();
    }
}

void EssScriptableWidget::createLocalConsole()
{
    m_localConsole = new QTextEdit();
    m_localConsole->setReadOnly(true);
    m_localConsole->setMaximumHeight(120); // Keep it compact in some layouts
    m_localConsole->setFont(QFont("Monaco", 9));
    m_localConsole->setStyleSheet(
        "QTextEdit {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  border: 1px solid #555;"
        "  font-family: 'Monaco', 'Consolas', monospace;"
        "}"
    );
}

void EssScriptableWidget::createCommandLine()
{
    m_commandLine = new QLineEdit();
    m_commandLine->setPlaceholderText("Enter Tcl command...");
    m_commandLine->setFont(QFont("Monaco", 10));
    m_commandLine->setStyleSheet(
        "QLineEdit {"
        "  background-color: #2b2b2b;"
        "  color: #ffffff;"
        "  border: 1px solid #555;"
        "  padding: 4px;"
        "  font-family: 'Monaco', 'Consolas', monospace;"
        "}"
    );
    
    // Connect to execute command on Enter
    connect(m_commandLine, &QLineEdit::returnPressed, this, &EssScriptableWidget::onCommandLineExecute);
    
    // Add command history with Up/Down arrows
    connect(m_commandLine, &QLineEdit::textChanged, this, &EssScriptableWidget::onCommandLineTextChanged);
    m_commandLine->installEventFilter(this);
}

void EssScriptableWidget::onCommandLineExecute()
{
    QString command = m_commandLine->text().trimmed();
    if (command.isEmpty()) return;
    
    // Add to history
    if (m_commandHistory.isEmpty() || m_commandHistory.last() != command) {
        m_commandHistory.append(command);
        if (m_commandHistory.size() > 50) {  // Keep last 50 commands
            m_commandHistory.removeFirst();
        }
    }
    m_historyIndex = m_commandHistory.size();
    
    // Log the command
    localLog(QString("> %1").arg(command));
    
    // Execute the command
    int result = eval(command);
    QString output = this->result();
    
    // Log the result
    if (result == TCL_OK) {
        if (!output.isEmpty()) {
            localLog(output);
        }
    } else {
        localLog(QString("ERROR: %1").arg(output));
    }
    
    // Clear the command line
    m_commandLine->clear();
}

void EssScriptableWidget::onCommandLineTextChanged()
{
    // Reset history index when user types
    if (!m_commandLine->text().isEmpty()) {
        m_historyIndex = m_commandHistory.size();
    }
}

// Add to eventFilter for Up/Down arrow keys
bool EssScriptableWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_commandLine && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        
        if (keyEvent->key() == Qt::Key_Up) {
            // Previous command in history
            if (m_historyIndex > 0) {
                m_historyIndex--;
                m_commandLine->setText(m_commandHistory[m_historyIndex]);
            }
            return true;
        } else if (keyEvent->key() == Qt::Key_Down) {
            // Next command in history
            if (m_historyIndex < m_commandHistory.size() - 1) {
                m_historyIndex++;
                m_commandLine->setText(m_commandHistory[m_historyIndex]);
            } else if (m_historyIndex == m_commandHistory.size() - 1) {
                m_historyIndex++;
                m_commandLine->clear();
            }
            return true;
        }
    }
    
    return QWidget::eventFilter(obj, event);
}

void EssScriptableWidget::localLog(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    
    // Log to console if in development mode
    if (m_localConsole && m_developmentMode) {
        m_localConsole->append(logLine);
        
        // Auto-scroll to bottom
        QTextCursor cursor = m_localConsole->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_localConsole->setTextCursor(cursor);
    }
    
    // Also log to main console for debugging
    EssConsoleManager::instance()->logDebug(logLine, QString("Widget:%1").arg(m_name));
}

// Slots
void EssScriptableWidget::onDevelopmentModeToggled(bool enabled)
{
    setDevelopmentMode(enabled);
}

void EssScriptableWidget::onLayoutModeChanged()
{
    if (m_layoutModeCombo) {
        DevLayoutMode mode = static_cast<DevLayoutMode>(m_layoutModeCombo->currentData().toInt());
        setDevelopmentLayout(mode);
    }
}

void EssScriptableWidget::onTestScript()
{
    if (!m_scriptEditor) return;
    
    localLog("=== Testing script ===");
    m_setupScript = m_scriptEditor->content();
    executeSetupScript();
}

void EssScriptableWidget::onResetScript()
{
    m_setupScript = m_defaultSetupScript;
    if (m_scriptEditor) {
        m_scriptEditor->setContent(m_setupScript);
    }
    localLog("Script reset to default");
}

void EssScriptableWidget::onGenerateCode()
{
    QString componentType = getWidgetTypeName();
    QString script = m_setupScript;
    
    // Escape the script for C++ embedding
    script.replace("\\", "\\\\");
    script.replace("\"", "\\\"");
    script.replace("\n", "\\n\"\n    \"");
    
    QString embeddedCode = QString(R"cpp(
// Auto-generated embedded script for %1

%1::%1(QWidget* parent) 
    : EssScriptableWidget("%2", parent)
{
    // Set default setup script
    setSetupScript(R"tcl(
%3
)tcl");
}
)cpp").arg(componentType, m_name, m_setupScript);
    
    // Show in dialog
    QDialog dialog(this);
    dialog.setWindowTitle("Generated C++ Code");
    dialog.resize(800, 600);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    EssCodeEditor* codeEditor = new EssCodeEditor(&dialog);
    codeEditor->setLanguage(EssCodeEditor::Cpp);
    codeEditor->setContent(embeddedCode);
    codeEditor->setReadOnly(true);
    layout->addWidget(codeEditor);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* copyButton = new QPushButton("Copy to Clipboard");
    QPushButton* closeButton = new QPushButton("Close");
    buttonLayout->addWidget(copyButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);
    
    connect(copyButton, &QPushButton::clicked, [embeddedCode]() {
        QApplication::clipboard()->setText(embeddedCode);
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    
    dialog.exec();
}

// Static Tcl command implementations
int EssScriptableWidget::tcl_bind_datapoint(ClientData clientData, Tcl_Interp* interp,
                                            int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "datapoint_pattern script");
        return TCL_ERROR;
    }
    
    QString pattern = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString script = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    widget->bindDatapoint(pattern, script);
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj("binding created", -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_shared_dg(ClientData clientData, Tcl_Interp* interp,
                                       int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    if (!widget->m_mainInterp) {
        Tcl_SetResult(interp, "No shared interpreter available", TCL_STATIC);
        return TCL_ERROR;
    }
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "dgname");
        return TCL_ERROR;
    }
    
    const char* dgName = Tcl_GetString(objv[1]);
    
    // Look up the DynGroup in the main interpreter
    DYN_GROUP* dg = nullptr;
    if (tclFindDynGroup(widget->m_mainInterp, const_cast<char*>(dgName), &dg) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("DynGroup not found", -1));
        return TCL_ERROR;
    }
    
    // Reference it in this interpreter
    if (tclPutDynGroup(interp, dg) != TCL_OK) {
        Tcl_SetResult(interp, "Failed to reference shared DynGroup", TCL_STATIC);
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(dgName, -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_local_log(ClientData clientData, Tcl_Interp* interp,
                                       int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "message");
        return TCL_ERROR;
    }
    
    QString message = QString::fromUtf8(Tcl_GetString(objv[1]));
    widget->localLog(message);
    
    return TCL_OK;
}

int EssScriptableWidget::tcl_test_datapoint(ClientData clientData, Tcl_Interp* interp,
                                            int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "datapoint_name value");
        return TCL_ERROR;
    }
    
    QString dpName = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString dpValue = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    widget->localLog(QString("Testing datapoint: %1 = %2").arg(dpName, dpValue));
    
    // Simulate the datapoint update with a slight delay
    QTimer::singleShot(100, [widget, dpName, dpValue]() {
        widget->onDatapointReceived_internal(dpName, QVariant(dpValue), 
                                           QDateTime::currentMSecsSinceEpoch());
    });
    
    return TCL_OK;
}


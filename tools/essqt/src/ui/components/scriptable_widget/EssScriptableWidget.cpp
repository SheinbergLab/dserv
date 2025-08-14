// EssScriptableWidget.cpp
#include "EssScriptableWidget.h"
#include "EssCodeEditor.h"
#include "EssWidgetTerminal.h"
#include "EssWidgetConsole.h"
#include "EssScriptableManager.h"
#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "core/EssEventProcessor.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QTextCursor>
#include <QMessageBox>
#include <QTimer>

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
    , m_devLayoutMode(DevThreePanel)
    , m_functionalWidget(nullptr)
    , m_scriptEditor(nullptr)
    , m_widgetTerminal(nullptr)
    , m_widgetConsole(nullptr)
    , m_devToolbar(nullptr)
    , m_splitter(nullptr)
    , m_tabWidget(nullptr)
    , m_floatingEditor(nullptr)
{
    initializeMainInterpreterReference();
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

void EssScriptableWidget::initializeMainInterpreterReference()
{
    // Get main interpreter from command interface
    auto* app = EssApplication::instance();
    if (app && app->commandInterface()) {
        m_mainInterp = app->commandInterface()->tclInterp();
        
        if (m_mainInterp) {
            localLog("Main interpreter reference set from command interface");
            
            // Optional: Test that it works
            if (Tcl_Eval(m_mainInterp, "info commands") == TCL_OK) {
                localLog("Main interpreter validation successful");
            }
        } else {
            localLog("WARNING: Command interface Tcl interpreter not ready yet");
            // Could set up a timer to retry, or defer until later
        }
    } else {
        localLog("WARNING: Application or command interface not available");
    }
}

void EssScriptableWidget::initializeCorePackages()
{
    if (!m_interp) return;
    
    // This should happen for ALL scriptable widgets
    // Load the core dlsh environment that all widgets need
    const char* corePackageScript = R"tcl(
        # Core package initialization for all scriptable widgets
        set f [file dirname [info nameofexecutable]]
        if { [file exists [file join $f dlsh.zip]] } { 
            set dlshzip [file join $f dlsh.zip] 
        } else {
            set dlshzip /usr/local/dlsh/dlsh.zip
        }
        set dlshroot [file join [zipfs root] dlsh]
        zipfs unmount $dlshroot
        zipfs mount $dlshzip $dlshroot
        set ::auto_path [linsert $::auto_path 0 [file join $dlshroot/lib]]
        
        # Load core packages that all widgets need
        package require dlsh
        
        # Set up common Tcl environment
        proc widget_log {msg} {
            local_log $msg
        }
        
        # Common utility procedures
        proc safe_eval {script} {
            if {[catch {eval $script} result]} {
                local_log "Script error: $result"
                return -code error $result
            }
            return $result
        }
    )tcl";
    
    if (Tcl_Eval(m_interp, corePackageScript) != TCL_OK) {
        localLog(QString("Warning: Core package initialization failed: %1").arg(result()));
    } else {
        localLog("Core Tcl packages loaded successfully");
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
    
    // Load core packages for all widgets
    initializeCorePackages();
    
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
    // Layout mode selector - only the three actual layouts
    m_devToolbar->addWidget(new QLabel("Layout:"));
    m_layoutModeCombo = new QComboBox();
    m_layoutModeCombo->addItem("Bottom Panel", DevBottomPanel);
    m_layoutModeCombo->addItem("Tabbed", DevTabbed);  
    m_layoutModeCombo->addItem("Three Panel", DevThreePanel);
    
    // Set to current preference
    m_layoutModeCombo->blockSignals(true);
    m_layoutModeCombo->setCurrentIndex(m_layoutModeCombo->findData(m_devLayoutMode));
    m_layoutModeCombo->blockSignals(false);
    
    connect(m_layoutModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &EssScriptableWidget::onLayoutModeChanged);
    m_devToolbar->addWidget(m_layoutModeCombo);
    
    m_devToolbar->addSeparator();
    
    // NEW: Prototype management
    m_devToolbar->addWidget(new QLabel("Prototype:"));
    m_prototypeSelector = new QComboBox();
    m_prototypeSelector->setMinimumWidth(120);
    connect(m_prototypeSelector, &QComboBox::currentTextChanged, 
            this, &EssScriptableWidget::loadPrototype);
    m_devToolbar->addWidget(m_prototypeSelector);
    
    m_savePrototypeAction = m_devToolbar->addAction("Save As");
    m_savePrototypeAction->setToolTip("Save current script as new prototype (with name/description prompts)");
    connect(m_savePrototypeAction, &QAction::triggered, this, &EssScriptableWidget::onSavePrototypeRequested);
    
    // NEW: Quick save action with keyboard shortcut
    m_quickSaveAction = new QAction("Quick Save", this);
    m_quickSaveAction->setShortcut(QKeySequence::Save);  // Ctrl+S
    m_quickSaveAction->setToolTip("Quick save to current prototype (Ctrl+S)");
    connect(m_quickSaveAction, &QAction::triggered, this, &EssScriptableWidget::onQuickSaveRequested);
    addAction(m_quickSaveAction);  // Add to widget for keyboard shortcut
    
    m_loadPrototypeAction = m_devToolbar->addAction("Load");  
    m_loadPrototypeAction->setToolTip("Load saved prototype");
    connect(m_loadPrototypeAction, &QAction::triggered, this, &EssScriptableWidget::onLoadPrototypeRequested);
    
    m_markProductionAction = m_devToolbar->addAction("Mark Prod");
    m_markProductionAction->setToolTip("Mark current prototype as production ready");
    connect(m_markProductionAction, &QAction::triggered, this, &EssScriptableWidget::onMarkProductionRequested);
    
    m_devToolbar->addSeparator();
    
    // Script actions
    m_testScriptAction = m_devToolbar->addAction("Test");
    m_testScriptAction->setToolTip("Execute current script");
    connect(m_testScriptAction, &QAction::triggered, this, &EssScriptableWidget::onTestScript);
    
    m_resetScriptAction = m_devToolbar->addAction("Reset");
    m_resetScriptAction->setToolTip("Reset to default script");
    connect(m_resetScriptAction, &QAction::triggered, this, &EssScriptableWidget::onResetScript);
    
    // Interpreter management actions
    m_devToolbar->addSeparator();
    
    m_resetInterpreterAction = m_devToolbar->addAction("Reset Tcl");
    m_resetInterpreterAction->setToolTip("Reset Tcl interpreter to clean state");
    connect(m_resetInterpreterAction, &QAction::triggered, this, &EssScriptableWidget::onResetInterpreterRequested);
    
    m_testFromScratchAction = m_devToolbar->addAction("Test Clean");
    m_testFromScratchAction->setToolTip("Reset interpreter and test script from scratch");
    connect(m_testFromScratchAction, &QAction::triggered, this, &EssScriptableWidget::onTestFromScratchRequested);
    
    m_devToolbar->addSeparator();
    
    m_generateCodeAction = m_devToolbar->addAction("C++");
    m_generateCodeAction->setToolTip("Generate C++ code");
    connect(m_generateCodeAction, &QAction::triggered, this, &EssScriptableWidget::onGenerateCode);
    
    // Status indicators
    m_prototypeStatusLabel = new QLabel();
    m_prototypeStatusLabel->setStyleSheet("QLabel { color: #666; font-style: italic; margin-left: 10px; }");
    m_devToolbar->addWidget(m_prototypeStatusLabel);
    
    m_interpreterStatusLabel = new QLabel();
    m_interpreterStatusLabel->setStyleSheet("QLabel { color: #0066cc; font-weight: bold; }");
    m_devToolbar->addWidget(m_interpreterStatusLabel);
        
    updatePrototypeUI();
}

void EssScriptableWidget::registerCoreCommands()
{
    if (!m_interp) return;
    
    // Core commands available to all scriptable widgets
    Tcl_CreateObjCommand(m_interp, "bind_datapoint", tcl_bind_datapoint, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "get_dg", tcl_get_dg, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "put_dg", tcl_put_dg, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "local_log", tcl_local_log, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "test_datapoint", tcl_test_datapoint, this, nullptr);
    
    // Add new event commands
    Tcl_CreateObjCommand(m_interp, "bind_event", tcl_bind_event, this, nullptr);
    Tcl_CreateObjCommand(m_interp, "list_event_types", tcl_list_event_types, 
    	this, nullptr);
    Tcl_CreateObjCommand(m_interp, "list_event_subtypes", tcl_list_event_subtypes,
    	this, nullptr);
    Tcl_CreateObjCommand(m_interp, "test_event", tcl_test_event, this, nullptr);

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
    
    auto* eventProcessor = processor->eventProcessor();
    if (eventProcessor) {
        connect(eventProcessor, &EssEventProcessor::eventReceived,
                this, &EssScriptableWidget::onEventReceived);
    }


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

bool EssScriptableWidget::eventFilter(QObject* obj, QEvent* event)
{
    // Pass through to parent
    return QWidget::eventFilter(obj, event);
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

void EssScriptableWidget::onEventReceived(const EssEvent& event)
{
    // Check for matching event bindings
    for (auto it = m_eventBindings.begin(); it != m_eventBindings.end(); ++it) {
        QString pattern = it.key();
        
        if (matchesEventPattern(pattern, event)) {
            QString script = substituteEventData(it.value(), event);
            eval(script);
        }
    }
}

bool EssScriptableWidget::matchesEventPattern(const QString& pattern, const EssEvent& event) const
{
    if (pattern == "*") return true;
    
    auto* eventProc = getEventProcessor();
    if (!eventProc) {
        // Fall back to numeric only
        return matchesNumericPattern(pattern, event);
    }
    
    if (pattern.contains(':')) {
        QStringList parts = pattern.split(':');
        if (parts.size() == 2) {
            QString typePart = parts[0];
            QString subtypePart = parts[1];
            
            // Try type by name first, then by number
            uint8_t typeId = 255;
            if (eventProc->isValidEventTypeName(typePart)) {
                typeId = eventProc->getEventTypeId(typePart);
            } else {
                bool ok;
                typeId = typePart.toUInt(&ok);
                if (!ok) typeId = 255;
            }
            
            if (typeId == 255 || typeId != event.type) {
                return false;
            }
            
            // Handle wildcard subtype
            if (subtypePart == "*") {
                return true;
            }
            
            // Try subtype by name first, then by number
            if (eventProc->isValidEventSubtypeName(typeId, subtypePart)) {
                auto pair = eventProc->getEventSubtypeId(typeId, subtypePart);
                return pair.second == event.subtype;
            } else {
                bool ok;
                uint8_t subtypeId = subtypePart.toUInt(&ok);
                return ok && subtypeId == event.subtype;
            }
        }
    } else {
        // Type-only pattern
        if (eventProc->isValidEventTypeName(pattern)) {
            return eventProc->getEventTypeId(pattern) == event.type;
        } else {
            bool ok;
            uint8_t typeId = pattern.toUInt(&ok);
            return ok && typeId == event.type;
        }
    }
    
    return false;
}

QString EssScriptableWidget::substituteEventData(const QString& script, const EssEvent& event) const
{
    QString result = script;
    
    // Basic event variables
    result.replace("$event_type", QString::number(event.type));
    result.replace("$event_subtype", QString::number(event.subtype));
    result.replace("$event_timestamp", QString::number(event.timestamp));
    result.replace("$event_params", event.paramsAsString());
    
    // Add friendly names if available
    auto* eventProc = getEventProcessor();
    if (eventProc) {
        result.replace("$event_type_name", eventProc->getEventTypeName(event.type));
        result.replace("$event_subtype_name", eventProc->getEventSubtypeName(event.type, event.subtype));
        
        // Add formatted name for logging
        QString friendlyName = QString("%1:%2")
            .arg(eventProc->getEventTypeName(event.type))
            .arg(eventProc->getEventSubtypeName(event.type, event.subtype));
        result.replace("$event_friendly_name", friendlyName);
    }
    
    // Widget info
    result.replace("$widget_name", m_name);
    result.replace("$widget_type", getWidgetTypeName());
    
    return result;
}

EssEventProcessor* EssScriptableWidget::getEventProcessor() const
{
    auto* app = EssApplication::instance();
    if (app && app->dataProcessor()) {
        return app->dataProcessor()->eventProcessor();
    }
    return nullptr;
}

bool EssScriptableWidget::matchesNumericPattern(const QString& pattern, const EssEvent& event) const
{
    if (pattern == "*") return true;
    
    QStringList parts = pattern.split(':');
    
    if (parts.size() == 1) {
        // Just type matching (numeric)
        bool ok;
        uint8_t typeNum = parts[0].toUInt(&ok);
        return ok && typeNum == event.type;
    } else if (parts.size() == 2) {
        // Type and subtype matching (numeric)
        bool ok1, ok2;
        uint8_t typeNum = parts[0].toUInt(&ok1);
        
        if (parts[1] == "*") {
            return ok1 && typeNum == event.type;
        }
        
        uint8_t subtypeNum = parts[1].toUInt(&ok2);
        return ok1 && ok2 && typeNum == event.type && subtypeNum == event.subtype;
    }
    
    return false;
}

void EssScriptableWidget::setDevelopmentMode(bool enabled)
{
    if (m_developmentMode == enabled) return;
    
    m_developmentMode = enabled;
    m_devToolbar->setVisible(enabled);
    
    if (enabled) {
        // Create components and apply current layout preference
        if (!m_scriptEditor) createScriptEditor();
        if (!m_widgetTerminal) createWidgetTerminal();  
        if (!m_widgetConsole) createWidgetConsole();
        
        // Apply the user's preferred development layout
        applyDevelopmentLayout();
        
        localLog("Development mode enabled");
    } else {
        // DISABLE: Clean up development UI but preserve layout preference
        cleanupDevelopmentLayout();
        localLog("Development mode disabled");
    }
    
    m_toggleDevModeAction->setChecked(enabled);
}

void EssScriptableWidget::cleanupDevelopmentLayout()
{
    // Clean up current development layout WITHOUT changing m_devLayoutMode
    if (m_splitter) {
        while (m_splitter->count() > 0) {
            QWidget* widget = m_splitter->widget(0);
            widget->setParent(nullptr);
        }
        m_splitter->setVisible(false);
        m_mainLayout->removeWidget(m_splitter);
        m_splitter->deleteLater();
        m_splitter = nullptr;
    }
    
    if (m_tabWidget) {
        while (m_tabWidget->count() > 0) {
            QWidget* widget = m_tabWidget->widget(0);
            m_tabWidget->removeTab(0);
            widget->setParent(nullptr);
        }
        m_tabWidget->setVisible(false);
        m_mainLayout->removeWidget(m_tabWidget);
        m_tabWidget->deleteLater();
        m_tabWidget = nullptr;
    }
    
    // Restore functional widget to main layout
    if (m_functionalWidget && m_functionalWidget->parent() != this) {
        m_functionalWidget->setParent(this);
        m_mainLayout->addWidget(m_functionalWidget);
    }
}

void EssScriptableWidget::setDevelopmentLayout(DevLayoutMode mode)
{
    if (m_devLayoutMode == mode) return;
    
    // Only allow layout changes when development mode is enabled
    if (!m_developmentMode) {
        localLog("Cannot change layout - development mode is disabled");
        return;
    }
    
    // Clean up current layout
    cleanupDevelopmentLayout();
    
    // Set new mode
    m_devLayoutMode = mode;
    
    // Update combo box
    if (m_layoutModeCombo) {
        m_layoutModeCombo->blockSignals(true);
        m_layoutModeCombo->setCurrentIndex(m_layoutModeCombo->findData(mode));
        m_layoutModeCombo->blockSignals(false);
    }
    
    // Apply new layout
    applyDevelopmentLayout();
    
    QString modeStr;
    switch (mode) {
        case DevBottomPanel: modeStr = "Bottom Panel"; break;
        case DevTabbed: modeStr = "Tabbed"; break;
        case DevThreePanel: modeStr = "Three Panel"; break;
    }
    localLog(QString("Development layout changed to: %1").arg(modeStr));
}


void EssScriptableWidget::applyDevelopmentLayout()
{
    // Only apply layout if development mode is enabled
    if (!m_developmentMode) {
        return;
    }
    
    switch (m_devLayoutMode) {
        case DevBottomPanel:
            setupBottomPanelLayout();
            break;
        case DevTabbed:
            setupTabbedLayout();
            break;
        case DevThreePanel:
            setupThreePanelLayout();
            break;
    }
}

void EssScriptableWidget::setupThreePanelLayout()
{
    if (!m_scriptEditor || !m_widgetTerminal || !m_widgetConsole) {
        localLog(QString("ERROR: Missing widgets - Script: %1, Terminal: %2, Console: %3")
                .arg(m_scriptEditor ? "OK" : "NULL")
                .arg(m_widgetTerminal ? "OK" : "NULL")
                .arg(m_widgetConsole ? "OK" : "NULL"));
        return;
    }
    
    // Create main vertical splitter for the three sections
    m_splitter = new QSplitter(Qt::Vertical, this);
    
    // Remove functional widget from main layout and add to splitter
    m_mainLayout->removeWidget(m_functionalWidget);
    m_functionalWidget->setParent(m_splitter);
    m_splitter->addWidget(m_functionalWidget);
    
    // Add script editor as second panel
    m_scriptEditor->setParent(m_splitter);
    m_splitter->addWidget(m_scriptEditor);
    
    // Create bottom panel with horizontal split for terminal and console
    QWidget* bottomPanel = new QWidget(m_splitter);
    QHBoxLayout* bottomLayout = new QHBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(2);
    
    // Create horizontal splitter for terminal and console
    QSplitter* bottomSplitter = new QSplitter(Qt::Horizontal, bottomPanel);
    
    // Add terminal and console to horizontal splitter
    m_widgetTerminal->setParent(bottomSplitter);
    m_widgetConsole->setParent(bottomSplitter);
    bottomSplitter->addWidget(m_widgetTerminal);
    bottomSplitter->addWidget(m_widgetConsole);
    
    // Set equal proportions for terminal and console
    bottomSplitter->setStretchFactor(0, 1);
    bottomSplitter->setStretchFactor(1, 1);
    
    bottomLayout->addWidget(bottomSplitter);
    m_splitter->addWidget(bottomPanel);
    
    // IMPORTANT: Set proportions that ensure graphics gets priority
    m_splitter->setStretchFactor(0, 6);  // Main widget gets 60% priority
    m_splitter->setStretchFactor(1, 2);  // Script editor gets 20%  
    m_splitter->setStretchFactor(2, 2);  // Terminal + Console get 20%
    
    // Set ONLY essential minimum sizes to prevent unusable states
    m_functionalWidget->setMinimumHeight(150);  // Graphics needs to be usable
    m_scriptEditor->setMinimumHeight(60);       // Script editor needs basic visibility
    bottomPanel->setMinimumHeight(60);          // Terminal needs basic visibility
    
    // NO maximum sizes - let user decide what they want
    
    // Set collapsible behavior - graphics widget should NOT be collapsible
    m_splitter->setCollapsible(0, false);  // Graphics cannot be collapsed
    m_splitter->setCollapsible(1, true);   // Script editor can be collapsed
    m_splitter->setCollapsible(2, true);   // Terminal area can be collapsed
    
    m_mainLayout->addWidget(m_splitter);
    
    // Make all widgets visible
    m_functionalWidget->setVisible(true);
    m_scriptEditor->setVisible(true);
    m_widgetTerminal->setVisible(true);
    m_widgetConsole->setVisible(true);
    bottomPanel->setVisible(true);
    m_splitter->setVisible(true);
}

void EssScriptableWidget::setupBottomPanelLayout()
{
    if (!m_scriptEditor || !m_widgetTerminal || !m_widgetConsole) return;
    
    // Create vertical splitter
    m_splitter = new QSplitter(Qt::Vertical, this);
    
    // Remove functional widget from main layout and add to splitter
    m_mainLayout->removeWidget(m_functionalWidget);
    m_functionalWidget->setParent(m_splitter);
    m_splitter->addWidget(m_functionalWidget);
    
    // Create bottom panel with tabs for development tools
    QWidget* bottomPanel = new QWidget(m_splitter);
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(2);
    
    // Create tabbed development tools
    QTabWidget* bottomTabs = new QTabWidget(bottomPanel);
    m_scriptEditor->setParent(bottomTabs);
    m_widgetTerminal->setParent(bottomTabs);
    m_widgetConsole->setParent(bottomTabs);
    bottomTabs->addTab(m_scriptEditor, "Script");
    bottomTabs->addTab(m_widgetTerminal, "Terminal");
    bottomTabs->addTab(m_widgetConsole, "Console");
    
    bottomLayout->addWidget(bottomTabs);
    m_splitter->addWidget(bottomPanel);
    
    // Set proportions
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);
    
    m_mainLayout->addWidget(m_splitter);
}

void EssScriptableWidget::setupTabbedLayout()
{
    if (!m_scriptEditor || !m_widgetTerminal || !m_widgetConsole) return;
    
    // Create main container for tabs
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
    m_widgetTerminal->setParent(m_tabWidget);
    m_widgetConsole->setParent(m_tabWidget);
    
    // Add tabs
    m_tabWidget->addTab(m_functionalWidget, getWidgetTypeName());
    m_tabWidget->addTab(m_scriptEditor, "Script");
    m_tabWidget->addTab(m_widgetTerminal, "Terminal");
    m_tabWidget->addTab(m_widgetConsole, "Console");
    
    containerLayout->addWidget(m_tabWidget);
    m_mainLayout->addWidget(tabbedContainer);
}

void EssScriptableWidget::createScriptEditor()
{
    m_scriptEditor = new EssCodeEditor();
    m_scriptEditor->setLanguage(EssCodeEditor::Tcl);
    m_scriptEditor->setContent(m_setupScript.isEmpty() ? 
        "# Component setup script\n# Use 'bind_datapoint' or 'bind_event' to connect to data or events\n\nlocal_log \"Component initialized\"\n" : 
        m_setupScript);
    m_scriptEditor->setToolbarVisible(false); // Keep it compact
    
    connect(m_scriptEditor, &EssCodeEditor::contentChanged, 
            [this](const QString& content) {
                m_setupScript = content;
            });
    
    // Connect the editor's save signal to our quick save
    connect(m_scriptEditor, &EssCodeEditor::saveRequested,
            this, [this]() {
                localLog("Save triggered from script editor");
                onQuickSaveRequested();
            });
}

void EssScriptableWidget::createWidgetTerminal()
{
    if (m_widgetTerminal) {
        return; // Already created
    }
    
    m_widgetTerminal = new EssWidgetTerminal(this);
            
    localLog("Widget terminal created");
}

void EssScriptableWidget::createWidgetConsole()
{
    if (m_widgetConsole) {
        return; // Already created
    }
    
    m_widgetConsole = new EssWidgetConsole(this);
    
    // The console will automatically receive log messages through localLog()
    localLog("Widget console created");
}


void EssScriptableWidget::resetInterpreter()
{
    localLog("=== RESETTING TCL INTERPRETER ===");
    
    // Save current script content
    QString currentScript = getSetupScript();
    
    // Clear all current bindings
    m_datapointBindings.clear();
    m_eventBindings.clear();
    
    // Delete and recreate interpreter
    if (m_interp) {
        Tcl_DeleteInterp(m_interp);
        m_interp = nullptr;
        m_initialized = false;
    }
    
    // Reinitialize from scratch
    initializeInterpreter();
    
    // Restore script content but don't execute yet
    m_setupScript = currentScript;
    if (m_scriptEditor) {
        m_scriptEditor->setContent(m_setupScript);
    }
    
    localLog("Tcl interpreter reset complete - clean state ready");
    updatePrototypeUI();
}

bool EssScriptableWidget::validateInterpreterState() const
{
    if (!m_interp) {
        return false;
    }
    
    // Check that interpreter is in clean state
    if (!m_datapointBindings.isEmpty() || !m_eventBindings.isEmpty()) {
        return false;
    }
    
    // Test basic Tcl functionality
    if (Tcl_Eval(m_interp, "expr 1 + 1") != TCL_OK) {
        return false;
    }
    
    QString result = QString::fromUtf8(Tcl_GetStringResult(m_interp));
    if (result != "2") {
        return false;
    }
    
    // Check that our custom commands are registered
    if (Tcl_Eval(m_interp, "info commands local_log") != TCL_OK) {
        return false;
    }
    
    QString commands = QString::fromUtf8(Tcl_GetStringResult(m_interp));
    return !commands.isEmpty();
}

void EssScriptableWidget::updatePrototypeUI()
{
    if (m_interpreterStatusLabel) {
        QString status;
        if (m_initialized && validateInterpreterState()) {
            status = "✓ Clean";
        } else if (m_initialized) {
            status = "⚠ Active";
        } else {
            status = "✗ Error";
        }
        m_interpreterStatusLabel->setText(QString("Tcl: %1").arg(status));
    }
    
    if (m_prototypeStatusLabel) {
        QString status;
        if (m_currentPrototype.isProduction) {
            status = QString("Production: %1").arg(m_currentPrototypeName);
        } else if (!m_currentPrototypeName.isEmpty()) {
            status = QString("Prototype: %1").arg(m_currentPrototypeName);
        } else {
            status = "No prototype";
        }
        m_prototypeStatusLabel->setText(status);
    }
    
    // Update prototype selector - FIX: Block signals and avoid underscore issue
    if (m_prototypeSelector) {
        m_prototypeSelector->blockSignals(true);
        m_prototypeSelector->clear();
        m_prototypeSelector->addItem("");  // Empty option
        
        QStringList prototypes = getAvailablePrototypes();
        for (const QString& name : prototypes) {
            m_prototypeSelector->addItem(name);
        }
        
        if (!m_currentPrototypeName.isEmpty()) {
            int index = m_prototypeSelector->findText(m_currentPrototypeName);
            if (index >= 0) {
                m_prototypeSelector->setCurrentIndex(index);
            }
        }
        m_prototypeSelector->blockSignals(false);
    }
}

// Prototype management methods
void EssScriptableWidget::saveCurrentAsPrototype(const QString& name, const QString& description)
{
    ScriptPrototype prototype;
    prototype.name = name.isEmpty() ? QString("prototype_%1").arg(QDateTime::currentSecsSinceEpoch()) : name;
    prototype.description = description;
    prototype.content = getSetupScript();
    prototype.author = qgetenv("USER");
    if (prototype.author.isEmpty()) {
        prototype.author = "Unknown";
    }
    prototype.created = QDateTime::currentDateTime();
    prototype.modified = prototype.created;
    prototype.version = "1.0";
    prototype.tags << "development";
    
    if (EssScriptPrototypeStore::instance()->savePrototype(getWidgetTypeName(), prototype)) {
        m_currentPrototypeName = prototype.name;
        m_currentPrototype = prototype;
        localLog(QString("Prototype saved: %1").arg(prototype.name));
        updatePrototypeUI();
    } else {
        localLog(QString("Failed to save prototype: %1").arg(prototype.name));
    }
}

void EssScriptableWidget::loadPrototype(const QString& name)
{
    if (name.isEmpty()) {
        m_currentPrototypeName.clear();
        m_currentPrototype = ScriptPrototype();
        updatePrototypeUI();
        return;
    }
    
    ScriptPrototype prototype = EssScriptPrototypeStore::instance()->loadPrototype(getWidgetTypeName(), name);
    if (!prototype.name.isEmpty()) {
        m_currentPrototypeName = prototype.name;
        m_currentPrototype = prototype;
        
        // Update the script editor
        setSetupScript(prototype.content);
        
        localLog(QString("Prototype loaded: %1").arg(name));
        updatePrototypeUI();
    } else {
        localLog(QString("Failed to load prototype: %1").arg(name));
    }
}

QStringList EssScriptableWidget::getAvailablePrototypes() const
{
    return EssScriptPrototypeStore::instance()->listPrototypes(getWidgetTypeName());
}

void EssScriptableWidget::markCurrentAsProduction()
{
    if (m_currentPrototypeName.isEmpty()) {
        localLog("No current prototype to mark as production");
        return;
    }
    
    if (EssScriptPrototypeStore::instance()->markAsProduction(getWidgetTypeName(), m_currentPrototypeName)) {
        localLog(QString("Prototype '%1' marked as PRODUCTION READY").arg(m_currentPrototypeName));
        m_currentPrototype.isProduction = true;
        updatePrototypeUI();
        
        // Show the embeddable code
        onGenerateEmbeddableRequested();
    }
}

QString EssScriptableWidget::generateEmbeddableScript() const
{
    QString script = getSetupScript();
    
    // Escape for C++ string literal
    script.replace("\\", "\\\\");
    script.replace("\"", "\\\"");
    script.replace("\n", "\\n\"\n        \"");
    
    QString embeddable = QString(R"cpp(
// PRODUCTION SCRIPT - Generated from prototype: %1
// Date: %2
// Description: %3

const QString %4::m_productionScript = R"tcl(
%5
)tcl";

// Usage in constructor:
// setSetupScript(m_productionScript);
)cpp")
    .arg(m_currentPrototypeName)
    .arg(QDateTime::currentDateTime().toString())
    .arg(m_currentPrototype.description)
    .arg(getWidgetTypeName())
    .arg(getSetupScript());  // Use original script, not escaped version
    
    return embeddable;
}

void EssScriptableWidget::onGenerateEmbeddableRequested()
{
    QString embeddable = generateEmbeddableScript();
    
    // Show in dialog with copy button
    QDialog dialog(this);
    dialog.setWindowTitle("Production Script - Ready for Embedding");
    dialog.resize(900, 700);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    QLabel* info = new QLabel(QString("Generated production script for embedding in %1.cpp:").arg(getWidgetTypeName()));
    info->setStyleSheet("font-weight: bold; color: #0066cc; margin-bottom: 10px;");
    layout->addWidget(info);
    
    EssCodeEditor* codeEditor = new EssCodeEditor(&dialog);
    codeEditor->setLanguage(EssCodeEditor::Cpp);
    codeEditor->setContent(embeddable);
    codeEditor->setReadOnly(true);
    layout->addWidget(codeEditor);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    
    QPushButton* copyButton = new QPushButton("Copy to Clipboard");
    QPushButton* saveFileButton = new QPushButton("Save to File");
    QPushButton* closeButton = new QPushButton("Close");
    
    buttonLayout->addWidget(copyButton);
    buttonLayout->addWidget(saveFileButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);
    
    connect(copyButton, &QPushButton::clicked, [embeddable]() {
        QApplication::clipboard()->setText(embeddable);
    });
    
    connect(saveFileButton, &QPushButton::clicked, [this, embeddable]() {
        QString fileName = QFileDialog::getSaveFileName(this, "Save Production Script",
            QString("%1_production_script.cpp").arg(getWidgetTypeName().toLower()),
            "C++ Files (*.cpp *.h)");
        if (!fileName.isEmpty()) {
            QFile file(fileName);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                file.write(embeddable.toUtf8());
                localLog(QString("Production script saved to: %1").arg(fileName));
            }
        }
    });
    
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    
    dialog.exec();
}

// Prototype slot implementations
void EssScriptableWidget::onSavePrototypeRequested()
{
    bool ok;
    QString name = QInputDialog::getText(this, "Save Prototype", 
                                        "Prototype name:", QLineEdit::Normal, 
                                        m_currentPrototypeName, &ok);
    if (ok && !name.isEmpty()) {
        QString desc = QInputDialog::getText(this, "Save Prototype",
                                           "Description (optional):", QLineEdit::Normal,
                                           m_currentPrototype.description, &ok);
        if (ok) {
            saveCurrentAsPrototype(name, desc);
        }
    }
}

void EssScriptableWidget::triggerQuickSave()
{
    // This is called from the main window when Cmd+S is pressed
    // and this widget (or a child) has focus
    
    if (!m_developmentMode) {
        localLog("Save requested but development mode is disabled");
        emit statusMessage("Development mode disabled - cannot save", 2000);
        return;
    }
    
    localLog("Save triggered from main window (Cmd+S)");
    onQuickSaveRequested();
}

void EssScriptableWidget::onQuickSaveRequested()
{
    if (!m_currentPrototypeName.isEmpty()) {
        // Quick save to existing prototype
        m_currentPrototype.content = getSetupScript();
        m_currentPrototype.modified = QDateTime::currentDateTime();
        
        if (EssScriptPrototypeStore::instance()->savePrototype(getWidgetTypeName(), m_currentPrototype)) {
            localLog(QString("Quick saved to prototype: %1").arg(m_currentPrototypeName));
            
            // Show brief status message
            if (m_prototypeStatusLabel) {
                QString oldText = m_prototypeStatusLabel->text();
                m_prototypeStatusLabel->setText("✓ Saved!");
                m_prototypeStatusLabel->setStyleSheet("QLabel { color: #22aa22; font-style: italic; }");
                
                // Reset status after 2 seconds
                QTimer::singleShot(2000, [this, oldText]() {
                    if (m_prototypeStatusLabel) {
                        m_prototypeStatusLabel->setText(oldText);
                        m_prototypeStatusLabel->setStyleSheet("QLabel { color: #666; font-style: italic; }");
                    }
                });
            }
        } else {
            localLog(QString("Failed to quick save prototype: %1").arg(m_currentPrototypeName));
        }
    } else {
        // No current prototype - fall back to regular save with prompts
        localLog("No current prototype - opening save dialog");
        onSavePrototypeRequested();
    }
}

void EssScriptableWidget::onLoadPrototypeRequested()
{
    QStringList prototypes = getAvailablePrototypes();
    if (prototypes.isEmpty()) {
        QMessageBox::information(this, "Load Prototype", "No saved prototypes found.");
        return;
    }
    
    bool ok;
    QString selected = QInputDialog::getItem(this, "Load Prototype",
                                           "Select prototype to load:", 
                                           prototypes, 0, false, &ok);
    if (ok && !selected.isEmpty()) {
        loadPrototype(selected);
    }
}

void EssScriptableWidget::onMarkProductionRequested()
{
    if (m_currentPrototypeName.isEmpty()) {
        QMessageBox::information(this, "Mark as Production", 
                                "Please save the current script as a prototype first.");
        return;
    }
    
    QMessageBox::StandardButton reply = QMessageBox::question(this,
        "Mark as Production",
        QString("Mark prototype '%1' as production ready?\n\n"
                "This will generate embeddable C++ code for deployment.")
                .arg(m_currentPrototypeName),
        QMessageBox::Yes | QMessageBox::No);
        
    if (reply == QMessageBox::Yes) {
        markCurrentAsProduction();
    }
}

// NEW: Slot implementations
void EssScriptableWidget::onResetInterpreterRequested()
{
    QMessageBox::StandardButton reply = QMessageBox::question(this, 
        "Reset Tcl Interpreter",
        "This will reset the Tcl interpreter to a clean state.\n"
        "Current variable values and Tcl state will be lost.\n"
        "The script will not be re-executed automatically.\n\n"
        "Continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
        
    if (reply == QMessageBox::Yes) {
        resetInterpreter();
    }
}

void EssScriptableWidget::onTestFromScratchRequested()
{
    QMessageBox::StandardButton reply = QMessageBox::question(this,
        "Test From Clean State", 
        "This will reset the Tcl interpreter and re-execute the current script.\n"
        "This tests that your script works correctly from a fresh start.\n\n"
        "Continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
        
    if (reply == QMessageBox::Yes) {
        localLog("=== TESTING SCRIPT FROM CLEAN INTERPRETER ===");
        
        // Reset to completely clean state
        resetInterpreter();
        
        // Small delay to ensure reset is complete
        QTimer::singleShot(100, this, [this]() {
            // Validate clean state
            if (!validateInterpreterState()) {
                localLog("ERROR: Interpreter not in clean state after reset!");
                return;
            }
            
            localLog("Clean state validated - executing setup script...");
            
            // Execute the setup script
            executeSetupScript();
            
            // Log final state
            localLog("=== FROM-SCRATCH TEST COMPLETE ===");
            localLog(QString("Datapoint bindings: %1").arg(m_datapointBindings.size()));
            localLog(QString("Event bindings: %1").arg(m_eventBindings.size()));
            
            // Test basic functionality
            eval("local_log \"Test script execution successful\"");
            
            updatePrototypeUI();
        });
    }
}

void EssScriptableWidget::localLog(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    
    // Log to widget console if in development mode
    if (m_widgetConsole && m_developmentMode) {
        m_widgetConsole->logMessage(message, OutputType::Info);
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
        localLog(QString("Layout mode changed to: %1").arg(mode));  // Debug
        setDevelopmentLayout(mode);
    }
}

void EssScriptableWidget::onTestScript()
{
    if (!m_scriptEditor) return;
    
    localLog("=== Testing script ===");
    m_setupScript = m_scriptEditor->content();
    executeSetupScript();
    updatePrototypeUI();  
}

// Update existing onResetScript to also update UI  
void EssScriptableWidget::onResetScript()
{
    m_setupScript = m_defaultSetupScript;
    if (m_scriptEditor) {
        m_scriptEditor->setContent(m_setupScript);
    }
    localLog("Script reset to default");
    updatePrototypeUI();  
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

int EssScriptableWidget::tcl_get_dg(ClientData clientData, Tcl_Interp* interp,
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
    
	char* dgName = Tcl_GetString(objv[1]);
    
    // Look up the dg in the main interpreter
    DYN_GROUP* dg = nullptr;
    if (tclFindDynGroup(widget->m_mainInterp, dgName, &dg) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("dg not found", -1));
        return TCL_ERROR;
    }
    
    // Copy if found
    DYN_GROUP *copy_dg = dfuCopyDynGroup(dg, DYN_GROUP_NAME(dg));
	if (!copy_dg) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("error copying dg", -1));
        return TCL_ERROR;
	}
	
	// Delete if exists in current interp
    if (tclFindDynGroup(interp, dgName, NULL) == TCL_OK) {
        QString deleteCmd = QString("catch {dg_delete %1}").arg(dgName);
        Tcl_Eval(interp, deleteCmd.toUtf8().constData());
  	}
  
    // Add to this interpreter
    if (tclPutDynGroup(interp, copy_dg) != TCL_OK) {
        Tcl_SetResult(interp, "error adding copied dg", TCL_STATIC);
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(dgName, -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_put_dg(ClientData clientData, Tcl_Interp* interp,
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
    
	char* dgName = Tcl_GetString(objv[1]);
    
    // Look up the dg in the local interpreter
    DYN_GROUP* dg = nullptr;
    if (tclFindDynGroup(interp, dgName, &dg) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("dg not found", -1));
        return TCL_ERROR;
    }
    
    // Copy if found
    DYN_GROUP *copy_dg = dfuCopyDynGroup(dg, DYN_GROUP_NAME(dg));
	if (!copy_dg) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("error copying dg", -1));
        return TCL_ERROR;
	}
	
	// Delete if exists in current interp
    if (tclFindDynGroup(widget->m_mainInterp, dgName, NULL) == TCL_OK) {
        QString deleteCmd = QString("catch {dg_delete %1}").arg(dgName);
        Tcl_Eval(widget->m_mainInterp, deleteCmd.toUtf8().constData());
  	}
  
    // Add to this interpreter
    if (tclPutDynGroup(widget->m_mainInterp, copy_dg) != TCL_OK) {
        Tcl_SetResult(interp, "error adding copied dg", TCL_STATIC);
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

int EssScriptableWidget::tcl_bind_event(ClientData clientData, Tcl_Interp* interp,
                                        int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "event_pattern script");
        return TCL_ERROR;
    }
    
    QString pattern = QString::fromUtf8(Tcl_GetString(objv[1]));
    QString script = QString::fromUtf8(Tcl_GetString(objv[2]));
    
    widget->bindEvent(pattern, script);
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj("event binding created", -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_list_event_types(ClientData clientData, Tcl_Interp* interp,
                                              int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    auto* eventProc = widget->getEventProcessor();
    
    if (!eventProc) {
        Tcl_SetResult(interp, "Event processor not available", TCL_STATIC);
        return TCL_ERROR;
    }
    
    QStringList typeNames = eventProc->getAvailableEventTypeNames();
    
    widget->localLog(QString("Available event types:\n%1").arg(typeNames.join("\n")));
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(typeNames.join(" ").toUtf8().constData(), -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_list_event_subtypes(ClientData clientData, Tcl_Interp* interp,
                                                 int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    auto* eventProc = widget->getEventProcessor();
    
    if (!eventProc || objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "event_type");
        return TCL_ERROR;
    }
    
    QString typeStr = QString::fromUtf8(Tcl_GetString(objv[1]));
    
    // Look up type ID
    uint8_t typeId = 255;
    if (eventProc->isValidEventTypeName(typeStr)) {
        typeId = eventProc->getEventTypeId(typeStr);
    } else {
        bool ok;
        typeId = typeStr.toUInt(&ok);
        if (!ok) typeId = 255;
    }
    
    if (typeId == 255) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("Invalid event type", -1));
        return TCL_ERROR;
    }
    
    QStringList subtypeNames = eventProc->getAvailableEventSubtypeNames(typeId);
    
    widget->localLog(QString("Available subtypes for type %1:\n%2")
                    .arg(typeStr, subtypeNames.join("\n")));
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(subtypeNames.join(" ").toUtf8().constData(), -1));
    return TCL_OK;
}

int EssScriptableWidget::tcl_test_event(ClientData clientData, Tcl_Interp* interp,
                                        int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssScriptableWidget*>(clientData);
    
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "type subtype params");
        return TCL_ERROR;
    }
    
    uint8_t type = QString::fromUtf8(Tcl_GetString(objv[1])).toUInt();
    uint8_t subtype = QString::fromUtf8(Tcl_GetString(objv[2])).toUInt();
    QString params = QString::fromUtf8(Tcl_GetString(objv[3]));
    
    // Create test event
    EssEvent testEvent;
    testEvent.type = type;
    testEvent.subtype = subtype;
    testEvent.timestamp = QDateTime::currentMSecsSinceEpoch();
    testEvent.ptype = PTYPE_STRING;
    testEvent.params = QJsonValue(params);
    
    widget->localLog(QString("Testing event: %1:%2").arg(type).arg(subtype));
    
    // Simulate event with slight delay
    QTimer::singleShot(100, [widget, testEvent]() {
        widget->onEventReceived(testEvent);
    });
    
    return TCL_OK;
}
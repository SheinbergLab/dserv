#include "EssTestWidget.h"

EssTestWidget::EssTestWidget(QWidget* parent)
    : EssScriptableWidget("test_widget", parent)
    , m_counter(0)
{
    // Set a default script for testing
    setSetupScript(R"tcl(
# Test Widget Setup Script
local_log "Initializing test widget..."

# Set initial message
set_message "Hello from Tcl!"

# Set initial counter
set_counter 0

# Define some helper procedures
proc increment_counter {} {
    global counter
    if {![info exists counter]} {
        set counter 0
    }
    incr counter
    set_counter $counter
    add_to_log "Counter incremented to $counter"
}

proc reset_counter {} {
    global counter
    set counter 0
    set_counter 0
    add_to_log "Counter reset"
}

proc demo_datapoint_handling {} {
    add_to_log "=== Demo: Datapoint Handling ==="
    add_to_log "Binding to 'test_data' datapoint..."
    
    bind_datapoint "test_data" {
        add_to_log "Received test_data: $dpoint_value"
        set_message "Last data: $dpoint_value"
    }
    
    add_to_log "Try: test_datapoint test_data \"hello world\""
}

# Bind to some example datapoints
bind_datapoint "trialdg" {
    add_to_log "Trial data updated at $dpoint_timestamp"
    increment_counter
}

bind_datapoint "stimdg" {
    add_to_log "Stimulus data updated"
    set_message "Stimulus updated"
}

bind_datapoint "test_*" {
    add_to_log "Test datapoint: $dpoint_name = $dpoint_value"
}

# Add some demo commands to the log
add_to_log "=== Test Widget Ready ==="
add_to_log "Available commands:"
add_to_log "  increment_counter    - Increase counter"
add_to_log "  reset_counter        - Reset counter to 0"
add_to_log "  demo_datapoint_handling - Show datapoint demo"
add_to_log "  test_datapoint <name> <value> - Simulate datapoint"
add_to_log ""
add_to_log "Try these in the script editor!"

local_log "Test widget setup complete"
)tcl");

    initializeWidget();
}

void EssTestWidget::registerCustomCommands()
{
    if (!interpreter()) return;
    
    // Register test-specific commands
    Tcl_CreateObjCommand(interpreter(), "set_message", tcl_set_message, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "set_counter", tcl_set_counter, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "add_to_log", tcl_add_to_log, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "clear_log", tcl_clear_log, this, nullptr);
}

QWidget* EssTestWidget::createMainWidget()
{
    QWidget* mainWidget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(mainWidget);
    
    // Message display
    QGroupBox* messageGroup = new QGroupBox("Message Display");
    QVBoxLayout* messageLayout = new QVBoxLayout(messageGroup);
    
    m_messageLabel = new QLabel("No message set");
    m_messageLabel->setStyleSheet("QLabel { font-size: 14pt; color: #0066cc; padding: 8px; }");
    m_messageLabel->setAlignment(Qt::AlignCenter);
    messageLayout->addWidget(m_messageLabel);
    
    layout->addWidget(messageGroup);
    
    // Counter display
    QGroupBox* counterGroup = new QGroupBox("Counter");
    QHBoxLayout* counterLayout = new QHBoxLayout(counterGroup);
    
    m_counterLabel = new QLabel("0");
    m_counterLabel->setStyleSheet("QLabel { font-size: 24pt; font-weight: bold; color: #aa2222; }");
    m_counterLabel->setAlignment(Qt::AlignCenter);
    counterLayout->addWidget(m_counterLabel);
    
    layout->addWidget(counterGroup);
    
    // Interactive controls
    QGroupBox* controlGroup = new QGroupBox("Controls");
    QVBoxLayout* controlLayout = new QVBoxLayout(controlGroup);
    
    m_textEdit = new QLineEdit();
    m_textEdit->setPlaceholderText("Enter text here...");
    connect(m_textEdit, &QLineEdit::textChanged, this, &EssTestWidget::onTextChanged);
    controlLayout->addWidget(m_textEdit);
    
    m_testButton = new QPushButton("Test Button");
    connect(m_testButton, &QPushButton::clicked, this, &EssTestWidget::onButtonClicked);
    controlLayout->addWidget(m_testButton);
    
    layout->addWidget(controlGroup);
    
    // Log area
    QGroupBox* logGroup = new QGroupBox("Script Log");
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    
    m_logArea = new QTextEdit();
    m_logArea->setMaximumHeight(150);
    m_logArea->setReadOnly(true);
    m_logArea->setFont(QFont("Monaco", 9));
    logLayout->addWidget(m_logArea);
    
    layout->addWidget(logGroup);
    
    return mainWidget;
}

void EssTestWidget::onSetupComplete()
{
    localLog("Test widget setup completed - UI should be updated");
}

void EssTestWidget::onButtonClicked()
{
    // Execute Tcl command when button clicked
    eval("increment_counter");
    localLog("Test button clicked - counter incremented via Tcl");
}

void EssTestWidget::onTextChanged()
{
    QString text = m_textEdit->text();
    if (!text.isEmpty()) {
        QString cmd = QString("set_message {Text: %1}").arg(text);
        eval(cmd);
    }
}

// Tcl command implementations
int EssTestWidget::tcl_set_message(ClientData clientData, Tcl_Interp* interp,
                                   int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssTestWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "message");
        return TCL_ERROR;
    }
    
    QString message = QString::fromUtf8(Tcl_GetString(objv[1]));
    widget->m_messageLabel->setText(message);
    
    return TCL_OK;
}

int EssTestWidget::tcl_set_counter(ClientData clientData, Tcl_Interp* interp,
                                   int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssTestWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "count");
        return TCL_ERROR;
    }
    
    int count = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &count) != TCL_OK) {
        return TCL_ERROR;
    }
    
    widget->m_counter = count;
    widget->m_counterLabel->setText(QString::number(count));
    
    return TCL_OK;
}

int EssTestWidget::tcl_add_to_log(ClientData clientData, Tcl_Interp* interp,
                                  int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssTestWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "message");
        return TCL_ERROR;
    }
    
    QString message = QString::fromUtf8(Tcl_GetString(objv[1]));
    widget->m_logArea->append(message);
    
    // Auto-scroll to bottom
    QTextCursor cursor = widget->m_logArea->textCursor();
    cursor.movePosition(QTextCursor::End);
    widget->m_logArea->setTextCursor(cursor);
    
    return TCL_OK;
}

int EssTestWidget::tcl_clear_log(ClientData clientData, Tcl_Interp* interp,
                                 int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssTestWidget*>(clientData);
    
    Q_UNUSED(objc)
    Q_UNUSED(objv)
    
    widget->m_logArea->clear();
    
    return TCL_OK;
}

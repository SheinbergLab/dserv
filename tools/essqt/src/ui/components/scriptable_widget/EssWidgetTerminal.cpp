#include "EssWidgetTerminal.h"
#include "EssScriptableWidget.h"
#include "CommandHistory.h"

#include <QKeyEvent>
#include <QScrollBar>
#include <QMenu>
#include <QPalette>
#include <QApplication>
#include <QClipboard>
#include <QTextCursor>

EssWidgetTerminal::EssWidgetTerminal(EssScriptableWidget* parentWidget, QWidget *parent)
    : QPlainTextEdit(parent)
    , m_parentWidget(parentWidget)
    , m_prompt("> ")
    , m_promptPosition(0)
    , m_isExecutingCommand(false)
    , m_history(std::make_unique<CommandHistory>())
{
    init();
    setupConnections();
}

EssWidgetTerminal::~EssWidgetTerminal() = default;

QSize EssWidgetTerminal::minimumSizeHint() const
{
    return QSize(200, 80);
}

QSize EssWidgetTerminal::sizeHint() const
{
    return QSize(400, 120);
}

void EssWidgetTerminal::init()
{
    // Set compact terminal appearance
    QFont terminalFont("Monaco, Menlo, Courier New");
    terminalFont.setFixedPitch(true);
    terminalFont.setPointSize(9);  // Smaller for widget terminals
    setFont(terminalFont);
    
    // Light terminal theme for widget terminals
    QPalette p = palette();
    p.setColor(QPalette::Base, QColor(248, 249, 250));        // Very light gray
    p.setColor(QPalette::Text, QColor(33, 37, 41));           // Dark text
    p.setColor(QPalette::Highlight, QColor(0, 123, 255));     // Blue selection
    p.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(p);
    
    // Terminal behavior
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setUndoRedoEnabled(false);
    setMaximumBlockCount(1000); // Smaller history for widget terminals
    
    // Compact cursor
    setCursorWidth(1);
    
    // Welcome message
    if (m_parentWidget) {
        QString widgetType = m_parentWidget->getWidgetTypeName();
        QString widgetName = m_parentWidget->name();
        appendOutput(QString("%1 (%2) - Widget Terminal\n").arg(widgetName, widgetType), OutputType::System);
        appendOutput("Commands execute in this widget's Tcl interpreter\n\n", OutputType::Info);
    }
    
    updatePrompt();
}

void EssWidgetTerminal::setupConnections()
{
    if (m_parentWidget) {
        // Update prompt based on widget info
        QString widgetName = m_parentWidget->name();
        m_prompt = QString("%1> ").arg(widgetName.split('_').first()); // Use first part of name
        
        // Connect to widget's script execution results
        connect(m_parentWidget, &EssScriptableWidget::scriptExecuted,
                this, &EssWidgetTerminal::onWidgetScriptExecuted);
    }
}

void EssWidgetTerminal::executeCommand(const QString &command)
{
    if (m_isExecutingCommand) return;
    
    // Display command
    moveCursor(QTextCursor::End);
    insertPlainText(command);
    
    // Process it
    processCommand();
}

void EssWidgetTerminal::clearTerminal()
{
    clear();
    appendOutput("Terminal cleared\n\n", OutputType::System);
    updatePrompt();
}

void EssWidgetTerminal::logMessage(const QString &message, OutputType type)
{
    // Insert log message with timestamp
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    appendOutput(QString("[%1] %2\n").arg(timestamp, message), type);
}

void EssWidgetTerminal::processCommand()
{
    QString command = getCurrentCommand().trimmed();
    
    if (command.isEmpty()) {
        appendOutput("\n");
        updatePrompt();
        return;
    }
    
    appendOutput("\n");
    m_isExecutingCommand = true;
    
    // Add to history
    m_history->add(command);
    
    // Handle built-in commands
    if (command == "clear") {
        clearTerminal();
        m_isExecutingCommand = false;
        return;
    }
    
    if (command == "exit" || command == "quit") {
        appendOutput("Use 'close_terminal' to close this terminal, or disable development mode.\n", OutputType::Info);
        m_isExecutingCommand = false;
        updatePrompt();
        return;
    }
    
    if (command.startsWith("exit ") || command.startsWith("quit ")) {
        appendOutput("To close terminal: use 'close_terminal' or disable development mode.\n", OutputType::Info);
        m_isExecutingCommand = false;
        updatePrompt();
        return;
    }
        
    if (command == "help") {
        appendOutput("Widget Terminal Commands:\n", OutputType::Info);
        appendOutput("  clear                   - Clear terminal\n", OutputType::Info);
        appendOutput("  help                    - Show this help\n", OutputType::Info);
        appendOutput("  local_log \"message\"     - Log a message\n", OutputType::Info);
        appendOutput("  test_behavmon           - Test widget (if available)\n", OutputType::Info);
        appendOutput("\nWidget-specific commands:\n", OutputType::Info);
        if (m_parentWidget && m_parentWidget->getWidgetTypeName() == "EssBehavmonWidget") {
            appendOutput("  set_general_performance 75 100 20\n", OutputType::Info);
            appendOutput("  test_behavmon\n", OutputType::Info);
            appendOutput("  clear_behavmon_data\n", OutputType::Info);
        }
        appendOutput("\n");
        m_isExecutingCommand = false;
        updatePrompt();
        return;
    }
    
    // Execute command in widget's interpreter
    if (m_parentWidget && m_parentWidget->interpreter()) {
        int result = m_parentWidget->eval(command);
        QString output = m_parentWidget->result();
    } else {
        appendOutput("Error: Widget interpreter not available\n", OutputType::Error);
    }
    
    m_isExecutingCommand = false;
    updatePrompt();
}

void EssWidgetTerminal::onWidgetScriptExecuted(int result, const QString& output)
{
    // This is called when the widget executes a script
    // For command results, we show them directly in terminal
    // For other script executions, we let the console handle logging
    if (m_isExecutingCommand && !output.isEmpty()) {
        OutputType type = (result == TCL_OK) ? OutputType::Success : OutputType::Error;
        appendOutput(output + "\n", type);
    }
}

void EssWidgetTerminal::updatePrompt()
{
    moveCursor(QTextCursor::End);
    
    QTextCharFormat promptFormat;
    promptFormat.setForeground(QColor(0, 123, 255));  // Blue
    promptFormat.setFontWeight(QFont::Bold);
    
    QTextCursor cursor = textCursor();
    cursor.insertText(m_prompt, promptFormat);
    
    m_promptPosition = cursor.position();
    setTextCursor(cursor);
}

void EssWidgetTerminal::appendOutput(const QString &text, OutputType type)
{
    moveCursor(QTextCursor::End);
    
    QTextCharFormat format;
    switch (type) {
        case OutputType::Error:
            format.setForeground(QColor(220, 53, 69));   // Red
            break;
        case OutputType::Warning:
            format.setForeground(QColor(255, 193, 7));   // Yellow
            break;
        case OutputType::Success:
            format.setForeground(QColor(40, 167, 69));   // Green
            break;
        case OutputType::System:
            format.setForeground(QColor(108, 117, 125)); // Gray
            break;
        case OutputType::Info:
        default:
            format.setForeground(QColor(33, 37, 41));    // Dark gray
            break;
    }
    
    textCursor().insertText(text, format);
    
    // Auto-scroll to bottom
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void EssWidgetTerminal::handleCopy()
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        QString selectedText = cursor.selectedText();
        selectedText.replace(QChar::ParagraphSeparator, '\n');
        QApplication::clipboard()->setText(selectedText);
    }
}

void EssWidgetTerminal::handlePaste()
{
    ensureCursorInEditableArea();
    
    QString text = QApplication::clipboard()->text();
    if (!text.isEmpty()) {
        // For widget terminals, just insert single line
        // Remove newlines to keep it simple
        text.replace('\n', ' ').replace('\r', ' ');
        insertPlainText(text);
    }
}

void EssWidgetTerminal::keyPressEvent(QKeyEvent *event)
{
    if (m_isExecutingCommand) {
        return; // Ignore input while executing
    }
    
    Qt::KeyboardModifiers modifiers = event->modifiers();
    
    // Handle common shortcuts
    if (modifiers & Qt::ControlModifier) {
        switch (event->key()) {
            case Qt::Key_C:
                if (textCursor().hasSelection()) {
                    handleCopy();
                } else {
                    // Ctrl+C to cancel current line
                    appendOutput("^C\n");
                    updatePrompt();
                }
                event->accept();
                return;
                
            case Qt::Key_V:
                handlePaste();
                event->accept();
                return;
                
            case Qt::Key_A: // Beginning of line
                {
                    QTextCursor cursor = textCursor();
                    cursor.setPosition(m_promptPosition);
                    setTextCursor(cursor);
                    event->accept();
                    return;
                }
                
            case Qt::Key_E: // End of line
                {
                    moveCursor(QTextCursor::End);
                    event->accept();
                    return;
                }
                
            case Qt::Key_L: // Clear screen
                {
                    clearTerminal();
                    event->accept();
                    return;
                }
        }
    }
    
    // Regular key handling
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            processCommand();
            event->accept();
            return;
            
        case Qt::Key_Backspace:
            if (textCursor().position() <= m_promptPosition) {
                event->accept();
                return; // Don't delete prompt
            }
            break;
            
        case Qt::Key_Left:
            if (textCursor().position() <= m_promptPosition) {
                event->accept();
                return; // Don't move into prompt
            }
            break;
            
        case Qt::Key_Home:
            {
                QTextCursor cursor = textCursor();
                cursor.setPosition(m_promptPosition);
                setTextCursor(cursor);
                event->accept();
                return;
            }
            
        case Qt::Key_Up:
            navigateHistory(-1);
            event->accept();
            return;
            
        case Qt::Key_Down:
            navigateHistory(1);
            event->accept();
            return;
    }
    
    // Only ensure cursor position before actual text input
    if (!event->text().isEmpty() && event->text()[0].isPrint()) {
        ensureCursorInEditableArea();
    }
    
    QPlainTextEdit::keyPressEvent(event);
}

void EssWidgetTerminal::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);
    
    // Add copy action
    QAction *copyAction = menu->addAction(tr("Copy"));
    copyAction->setEnabled(textCursor().hasSelection());
    connect(copyAction, &QAction::triggered, this, &EssWidgetTerminal::handleCopy);
    
    // Add paste action
    QAction *pasteAction = menu->addAction(tr("Paste"));
    pasteAction->setEnabled(!QApplication::clipboard()->text().isEmpty());
    connect(pasteAction, &QAction::triggered, this, &EssWidgetTerminal::handlePaste);
    
    menu->addSeparator();
    
    QAction *clearAction = menu->addAction(tr("Clear Terminal"));
    connect(clearAction, &QAction::triggered, this, &EssWidgetTerminal::clearTerminal);
    
    // Widget-specific actions
    if (m_parentWidget) {
        menu->addSeparator();
        
        QAction *helpAction = menu->addAction(tr("Help"));
        connect(helpAction, &QAction::triggered, [this]() {
            executeCommand("help");
        });
        
        if (m_parentWidget->getWidgetTypeName() == "EssBehavmonWidget") {
            QAction *testAction = menu->addAction(tr("Test Widget"));
            connect(testAction, &QAction::triggered, [this]() {
                executeCommand("test_behavmon");
            });
        }
    }
    
    menu->exec(event->globalPos());
    delete menu;
}

void EssWidgetTerminal::ensureCursorInEditableArea()
{
    QTextCursor cursor = textCursor();
    if (cursor.position() < m_promptPosition) {
        cursor.setPosition(m_promptPosition);
        setTextCursor(cursor);
    }
}

QString EssWidgetTerminal::getCurrentCommand() const
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(m_promptPosition);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    return cursor.selectedText();
}

void EssWidgetTerminal::replaceCurrentCommand(const QString &newCommand)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(m_promptPosition);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(newCommand);
}

void EssWidgetTerminal::navigateHistory(int direction)
{
    // Save current command if we're starting to navigate
    if (m_history->currentIndex() == -1) {
        m_history->setTempCommand(getCurrentCommand());
    }
    
    QString historicalCommand;
    if (direction < 0) {
        historicalCommand = m_history->getPrevious();
    } else {
        historicalCommand = m_history->getNext();
    }
    
    if (!historicalCommand.isNull()) {
        replaceCurrentCommand(historicalCommand);
    }
}

void EssWidgetTerminal::setPrompt(const QString &prompt)
{
    m_prompt = prompt;
}
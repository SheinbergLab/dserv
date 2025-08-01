#include "EssTerminalWidget.h"
#include "CommandHistory.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"

#include <QKeyEvent>
#include <QTextBlock>
#include <QScrollBar>
#include <QMenu>
#include <QPalette>
#include <QDebug>
#include <QCompleter>
#include <QStringListModel>

EssTerminalWidget::EssTerminalWidget(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_prompt("ess> ")
    , m_promptPosition(0)
    , m_isExecutingCommand(false)
    , m_history(std::make_unique<CommandHistory>())
    , m_completer(nullptr)
{
    init();
    setupCommandInterface();
}

EssTerminalWidget::~EssTerminalWidget() = default;

void EssTerminalWidget::init()
{
    // Set terminal appearance
    QFont terminalFont("Monaco, Menlo, Courier New");
    terminalFont.setFixedPitch(true);
    terminalFont.setPointSize(10);
    setFont(terminalFont);
    
    // Dark terminal theme
    QPalette p = palette();
    p.setColor(QPalette::Base, QColor(40, 44, 52));        // Dark gray background
    p.setColor(QPalette::Text, QColor(171, 178, 191));     // Light gray text
    p.setColor(QPalette::Highlight, QColor(61, 90, 128));  // Selection color
    p.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(p);
    
    // Terminal behavior
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setUndoRedoEnabled(false);
    setMaximumBlockCount(10000); // Limit history
    
    // Cursor
    setCursorWidth(2);
    
    // Welcome message
    appendOutput("ESS Qt Terminal\n", OutputType::System);
    appendOutput("Type 'help' for available commands\n", OutputType::System);
    appendOutput("Local Tcl interpreter ready\n\n", OutputType::Success);
    
    // Initial prompt
    updatePrompt();
}

void EssTerminalWidget::setupCommandInterface()
{
    // Get command interface from application
    EssCommandInterface* cmdInterface = EssApplication::instance()->commandInterface();
    
    // Connect command interface signals
    connect(cmdInterface, &EssCommandInterface::connected,
            this, [this](const QString &host) {
                appendOutput(QString("Connected to %1\n").arg(host), OutputType::Success);
                updatePrompt();
            });
    
    connect(cmdInterface, &EssCommandInterface::disconnected,
            this, [this]() {
                appendOutput("Disconnected from host\n", OutputType::System);
                updatePrompt();
            });
    
    connect(cmdInterface, &EssCommandInterface::connectionError,
            this, [this](const QString &error) {
                appendOutput(QString("Connection error: %1\n").arg(error), OutputType::Error);
            });
    
    // Connect to built-in command signals
    connect(cmdInterface, &EssCommandInterface::clearRequested,
            this, &EssTerminalWidget::clearTerminal);
    
    // Set up command completion
    setupCompleter();
}

void EssTerminalWidget::setupCompleter()
{
    m_completer = new QCompleter(this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    
    // Update completion list
    updateCompletionList();
    
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &EssTerminalWidget::insertCompletion);
}

void EssTerminalWidget::updateCompletionList()
{
    QStringList commands;
    
    // Get Tcl commands from command interface
    commands = EssApplication::instance()->commandInterface()->getAvailableCommands();
    
    // Add our built-in commands
    commands << "connect" << "disconnect" << "subscribe" << "unsubscribe" 
             << "subscriptions" << "status" << "clear" << "help" << "about" 
             << "exit" << "quit" << "/local" << "/tcl" << "/ess" << "/dserv";
    
    // Remove duplicates and sort
    commands.removeDuplicates();
    commands.sort();
    
    m_completer->setModel(new QStringListModel(commands, m_completer));
}

void EssTerminalWidget::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    int extra = completion.length() - m_completer->completionPrefix().length();
    cursor.movePosition(QTextCursor::Left);
    cursor.movePosition(QTextCursor::EndOfWord);
    cursor.insertText(completion.right(extra));
    setTextCursor(cursor);
}

void EssTerminalWidget::executeCommand(const QString &command)
{
    if (m_isExecutingCommand) return;
    
    // Display command
    moveCursor(QTextCursor::End);
    insertPlainText(command);
    
    // Process it
    processCommand();
}

void EssTerminalWidget::clearTerminal()
{
    clear();
    appendOutput("Terminal cleared\n\n", OutputType::System);
    updatePrompt();
}

void EssTerminalWidget::processCommand()
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
    
    // Emit status message if needed
    emit statusMessage(QString("Executing: %1").arg(command), 2000);
    
    // Execute through command interface - it handles EVERYTHING
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    EssCommandInterface::CommandResult result = cmdInterface->executeCommand(command);
    
    // Display result
    switch (result.status) {
        case EssCommandInterface::StatusSuccess:
            if (!result.response.isEmpty()) {
                appendOutput(result.response + "\n", OutputType::Info);
            }
            break;
            
        case EssCommandInterface::StatusError:
            if (!result.error.isEmpty()) {
                appendOutput("Error: " + result.error + "\n", OutputType::Error);
            }
            break;
            
        case EssCommandInterface::StatusTimeout:
            appendOutput("Command timed out\n", OutputType::Warning);
            break;
            
        case EssCommandInterface::StatusNotConnected:
            appendOutput("Not connected. Use 'connect <host>' to connect.\n", OutputType::Warning);
            break;
    }
    
    m_isExecutingCommand = false;
    updatePrompt();
}

void EssTerminalWidget::updatePrompt(const QString &newPrompt)
{
    if (!newPrompt.isEmpty()) {
        m_prompt = newPrompt;
    } else {
        // Update prompt based on current channel and connection
        EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
        EssCommandInterface::CommandChannel channel = cmdInterface->defaultChannel();
        QString host = cmdInterface->currentHost();
        
        switch (channel) {
            case EssCommandInterface::ChannelLocal:
                m_prompt = "tcl> ";
                break;
                
            case EssCommandInterface::ChannelEss:
                m_prompt = host.isEmpty() ? "ess> " : QString("ess:%1> ").arg(host);
                break;
                
            case EssCommandInterface::ChannelDserv:
                m_prompt = host.isEmpty() ? "dserv> " : QString("dserv:%1> ").arg(host);
                break;
                
            default:
                m_prompt = "ess> ";
        }
    }
    
    moveCursor(QTextCursor::End);
    
    // Store where this line starts
    m_currentLineStart = textCursor().position();
    
    QTextCharFormat promptFormat;
    promptFormat.setForeground(QColor(97, 175, 239));  // Bright blue
    promptFormat.setFontWeight(QFont::Bold);
    
    QTextCursor cursor = textCursor();
    cursor.insertText(m_prompt, promptFormat);
    
    m_promptPosition = cursor.position();
    setTextCursor(cursor);
}

void EssTerminalWidget::appendOutput(const QString &text, OutputType type)
{
    moveCursor(QTextCursor::End);
    
    QTextCharFormat format;
    switch (type) {
        case OutputType::Error:
            format.setForeground(QColor(224, 108, 117));  // Soft red
            break;
        case OutputType::Warning:
            format.setForeground(QColor(255, 195, 0));    // Yellow/Orange
            break;
        case OutputType::Success:
            format.setForeground(QColor(87, 199, 135));   // Green
            break;
        case OutputType::System:
            format.setForeground(QColor(86, 182, 255));   // Blue
            break;
        case OutputType::Info:
        default:
            format.setForeground(QColor(171, 178, 191));  // Default gray
            break;
    }
    
    textCursor().insertText(text, format);
    
    // Auto-scroll to bottom
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void EssTerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_isExecutingCommand) {
        return; // Ignore input while executing
    }
    
    Qt::KeyboardModifiers modifiers = event->modifiers();
    
    // On macOS, we want to handle both Ctrl and Cmd for some shortcuts
    // but for Emacs bindings, we specifically want Ctrl
#ifdef Q_OS_MAC
    bool isCtrl = (modifiers & Qt::ControlModifier);
    bool isCmd = (modifiers & Qt::MetaModifier);
    
    // For Emacs key bindings, use actual Ctrl key
    if (isCtrl && !isCmd) {
#else
    // On other platforms, just use Ctrl
    if (modifiers & Qt::ControlModifier) {
#endif
        switch (event->key()) {
            case Qt::Key_A: // Ctrl+A - Beginning of line
                {
                    QTextCursor cursor = textCursor();
                    cursor.setPosition(m_promptPosition);
                    setTextCursor(cursor);
                    return;
                }
            case Qt::Key_E: // Ctrl+E - End of line
                {
                    moveCursor(QTextCursor::End);
                    return;
                }
                
            case Qt::Key_K: // Ctrl+K - Kill to end of line
                {
                    QTextCursor cursor = textCursor();
                    if (cursor.position() >= m_promptPosition) {
                        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                        cursor.removeSelectedText();
                    }
                    return;
                }
                
            case Qt::Key_U: // Ctrl+U - Kill to beginning of line
                {
                    QTextCursor cursor = textCursor();
                    if (cursor.position() > m_promptPosition) {
                        int currentPos = cursor.position();
                        cursor.setPosition(m_promptPosition);
                        cursor.setPosition(currentPos, QTextCursor::KeepAnchor);
                        cursor.removeSelectedText();
                    }
                    return;
                }
                
            case Qt::Key_W: // Ctrl+W - Kill word backward
                {
                    QTextCursor cursor = textCursor();
                    cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
                    if (cursor.position() < m_promptPosition) {
                        cursor.setPosition(m_promptPosition);
                        cursor.setPosition(textCursor().position(), QTextCursor::KeepAnchor);
                    }
                    cursor.removeSelectedText();
                    return;
                }
                
            case Qt::Key_D: // Ctrl+D - Delete character forward
                {
                    if (getCurrentCommand().isEmpty()) {
                        appendOutput("Use 'exit' or 'quit' to close\n");
                        updatePrompt();
                    } else {
                        QTextCursor cursor = textCursor();
                        cursor.deleteChar();
                    }
                    return;
                }
                
            case Qt::Key_L: // Ctrl+L - Clear screen
                {
                    clearTerminal();
                    return;
                }
                
            case Qt::Key_P: // Ctrl+P - Previous history
                {
                    navigateHistory(-1);
                    return;
                }
                
            case Qt::Key_N: // Ctrl+N - Next history
                {
                    navigateHistory(1);
                    return;
                }
                
            case Qt::Key_C: // Ctrl+C - Cancel current line
                {
                    appendOutput("^C\n");
                    updatePrompt();
                    return;
                }
        }
    }
    
    // Alt key bindings
    if (event->modifiers() & Qt::AltModifier) {
        switch (event->key()) {
            case Qt::Key_B: // Alt+B - Word backward
                {
                    QTextCursor cursor = textCursor();
                    cursor.movePosition(QTextCursor::PreviousWord);
                    if (cursor.position() < m_promptPosition) {
                        cursor.setPosition(m_promptPosition);
                    }
                    setTextCursor(cursor);
                    return;
                }
                
            case Qt::Key_F: // Alt+F - Word forward
                {
                    moveCursor(QTextCursor::NextWord);
                    return;
                }
                
            case Qt::Key_D: // Alt+D - Kill word forward
                {
                    QTextCursor cursor = textCursor();
                    cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                    return;
                }
        }
    }
    
    // Regular key handling
    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            processCommand();
            return;
            
        case Qt::Key_Backspace:
            if (textCursor().position() <= m_promptPosition) {
                return; // Don't delete prompt
            }
            break;
            
        case Qt::Key_Left:
            if (textCursor().position() <= m_promptPosition) {
                return; // Don't move into prompt
            }
            break;
            
        case Qt::Key_Home:
            {
                QTextCursor cursor = textCursor();
                cursor.setPosition(m_promptPosition);
                setTextCursor(cursor);
                return;
            }
            
        case Qt::Key_Up:
            navigateHistory(-1);
            return;
            
        case Qt::Key_Down:
            navigateHistory(1);
            return;
            
        case Qt::Key_Tab:
            {
                // Simple tab completion
                QString currentWord = getCurrentCommand();
                m_completer->setCompletionPrefix(currentWord);
                m_completer->complete();
                return;
            }
    }
    
    // Ensure we're at the end for typing
    ensureCursorInEditableArea();
    
    QPlainTextEdit::keyPressEvent(event);
}

void EssTerminalWidget::mousePressEvent(QMouseEvent *event)
{
    QPlainTextEdit::mousePressEvent(event);
    ensureCursorInEditableArea();
}

void EssTerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseDoubleClickEvent(event);
    ensureCursorInEditableArea();
}

void EssTerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu();
    
    menu->addSeparator();
    QAction *clearAction = menu->addAction(tr("Clear Terminal"));
    connect(clearAction, &QAction::triggered, this, &EssTerminalWidget::clearTerminal);
    
    menu->exec(event->globalPos());
    delete menu;
}

void EssTerminalWidget::ensureCursorInEditableArea()
{
    QTextCursor cursor = textCursor();
    if (cursor.position() < m_promptPosition) {
        cursor.setPosition(m_promptPosition);
        setTextCursor(cursor);
    }
}

QString EssTerminalWidget::getCurrentCommand() const
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(m_promptPosition);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    return cursor.selectedText();
}

void EssTerminalWidget::replaceCurrentCommand(const QString &newCommand)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(m_promptPosition);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(newCommand);
}

void EssTerminalWidget::navigateHistory(int direction)
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

void EssTerminalWidget::setPrompt(const QString &prompt)
{
    m_prompt = prompt;
    // Could update current prompt if needed
}

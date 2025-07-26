#include "TclConsoleWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QFont>
#include <QFontDatabase>
#include <QTextCursor>
#include <QDebug>

TclConsoleWidget::TclConsoleWidget(QWidget *parent) 
    : QWidget(parent), tclInterp(nullptr), historyIndex(-1) {
    setupUI();
    setupTclEnvironment();
}

TclConsoleWidget::~TclConsoleWidget() {
    if (tclInterp) {
        delete tclInterp;
    }
}

void TclConsoleWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    
    // Create splitter for output and script areas
    splitter = new QSplitter(Qt::Vertical);
    
    // Use system monospace font
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(10);
    
    // Output area (console output)
    outputArea = new QPlainTextEdit();
    outputArea->setReadOnly(true);
    outputArea->setFont(monoFont);
    outputArea->setMaximumBlockCount(1000); // Limit output history
    
    // Script area (multi-line editing)
    scriptArea = new QTextEdit();
    scriptArea->setFont(monoFont);
    scriptArea->setPlaceholderText("Enter multi-line Tcl scripts here...");
    
    splitter->addWidget(outputArea);
    splitter->addWidget(scriptArea);
    splitter->setSizes({300, 200}); // Initial sizes
    
    layout->addWidget(splitter);
    
    // Command input area
    auto* commandLayout = new QHBoxLayout();
    
    commandLine = new QLineEdit();
    commandLine->setFont(monoFont);
    commandLine->setPlaceholderText("Enter Tcl command...");
    
    executeButton = new QPushButton("Execute");
    clearButton = new QPushButton("Clear");
    loadScriptButton = new QPushButton("Load");
    saveScriptButton = new QPushButton("Save");
    
    commandLayout->addWidget(new QLabel("Command:"));
    commandLayout->addWidget(commandLine);
    commandLayout->addWidget(executeButton);
    commandLayout->addWidget(clearButton);
    commandLayout->addWidget(loadScriptButton);
    commandLayout->addWidget(saveScriptButton);
    
    layout->addLayout(commandLayout);
    
    // Status bar
    statusLabel = new QLabel("Ready");
    statusLabel->setStyleSheet("QLabel { color: green; }");
    layout->addWidget(statusLabel);
    
    // Connect signals
    connect(commandLine, &QLineEdit::returnPressed, this, &TclConsoleWidget::executeCommand);
    connect(executeButton, &QPushButton::clicked, this, &TclConsoleWidget::executeCommand);
    connect(clearButton, &QPushButton::clicked, this, &TclConsoleWidget::clearConsole);
    connect(loadScriptButton, &QPushButton::clicked, this, &TclConsoleWidget::loadScript);
    connect(saveScriptButton, &QPushButton::clicked, this, &TclConsoleWidget::saveScript);
}

void TclConsoleWidget::setupTclEnvironment() {
    try {
        char* argv[] = {(char*)"tcl_console", nullptr};
        tclInterp = new TclInterp(1, argv);
        
        // Initialize with useful commands
        QString initScript = R"(
            # ESS Tcl Console initialization
            proc gui_log {msg} {
                puts "GUI: $msg"
            }
            
            proc current_time {} {
                return [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]
            }
            
            # Math utilities commonly used in data analysis
            proc mean {list} {
                set sum 0.0
                set count 0
                foreach val $list {
                    set sum [expr {$sum + $val}]
                    incr count
                }
                return [expr {$sum / $count}]
            }
            
            proc stdev {list} {
                set m [mean $list]
                set sum 0.0
                set count 0
                foreach val $list {
                    set sum [expr {$sum + pow($val - $m, 2)}]
                    incr count
                }
                return [expr {sqrt($sum / ($count - 1))}]
            }
            
            gui_log "Tcl console environment initialized at [current_time]"
        )";
        
        evaluateCommand(initScript);
        appendOutput("Tcl interpreter initialized successfully");
        statusLabel->setText("Tcl Ready");
        statusLabel->setStyleSheet("QLabel { color: green; }");
        
    } catch (const std::exception& e) {
        appendOutput(QString("Failed to initialize Tcl: %1").arg(e.what()), true);
        statusLabel->setText("Tcl Error");
        statusLabel->setStyleSheet("QLabel { color: red; }");
    }
}

QString TclConsoleWidget::evaluateCommand(const QString& command) {
    if (!tclInterp) {
        return "Error: Tcl interpreter not available";
    }
    
    try {
        std::string result = tclInterp->eval(command.toUtf8().constData());
        return QString::fromStdString(result);
    } catch (const std::exception& e) {
        return QString("Tcl Error: %1").arg(e.what());
    }
}

bool TclConsoleWidget::evaluateCommandWithResult(const QString& command, QString& result) {
    if (!tclInterp) {
        result = "Error: Tcl interpreter not available";
        return false;
    }
    
    try {
        std::string resultStr;
        int returnCode = tclInterp->eval(command.toUtf8().constData(), resultStr);
        result = QString::fromStdString(resultStr);
        return (returnCode == TCL_OK);
    } catch (const std::exception& e) {
        result = QString("Tcl Error: %1").arg(e.what());
        return false;
    }
}

void TclConsoleWidget::executeCommand() {
    QString command = commandLine->text().trimmed();
    if (command.isEmpty()) {
        // If command line is empty, try to execute script area content
        command = scriptArea->toPlainText().trimmed();
        if (command.isEmpty()) return;
    }
    
    // Add to history
    if (!command.isEmpty() && (commandHistory.isEmpty() || commandHistory.last() != command)) {
        commandHistory.append(command);
        if (commandHistory.size() > 100) { // Limit history size
            commandHistory.removeFirst();
        }
    }
    historyIndex = commandHistory.size();
    
    // Display command
    appendOutput(QString("% %1").arg(command));
    
    // Execute command
    QString result;
    bool success = evaluateCommandWithResult(command, result);
    
    if (!result.isEmpty()) {
        appendOutput(result, !success);
    }
    
    // Emit signal for external listeners
    emit commandExecuted(command, result);
    if (!success) {
        emit errorOccurred(result);
    }
    
    // Clear command line
    commandLine->clear();
    
    // Clear script area if we executed from there
    if (commandLine->text().isEmpty()) {
        scriptArea->clear();
    }
}

void TclConsoleWidget::appendOutput(const QString& text, bool isError) {
    if (isError) {
        outputArea->appendHtml(QString("<span style='color: red;'>%1</span>").arg(text.toHtmlEscaped()));
    } else {
        outputArea->appendPlainText(text);
    }
    
    // Auto-scroll to bottom
    QTextCursor cursor = outputArea->textCursor();
    cursor.movePosition(QTextCursor::End);
    outputArea->setTextCursor(cursor);
}

void TclConsoleWidget::clearConsole() {
    outputArea->clear();
    appendOutput("Console cleared");
}

void TclConsoleWidget::putDynGroup(DYN_GROUP* dg) {
    if (tclInterp && dg) {
        int result = tclInterp->tclPutGroup(dg);
        if (result != TCL_OK) {
            appendOutput("Error: Failed to put DYN_GROUP into Tcl", true);
        }
    }
}

DYN_LIST* TclConsoleWidget::findDynList(DYN_GROUP* dg, const QString& name) {
    if (tclInterp && dg) {
        return tclInterp->findDynList(dg, name.toUtf8().data());
    }
    return nullptr;
}

void TclConsoleWidget::loadScript() {
    // TODO: Implement file dialog to load Tcl script
    statusLabel->setText("Load script - TODO");
}

void TclConsoleWidget::saveScript() {
    // TODO: Implement file dialog to save current script
    statusLabel->setText("Save script - TODO");
}

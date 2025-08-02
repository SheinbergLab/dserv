#include "EssOutputConsole.h"
#include <QMenu>
#include <QTimer>
#include <QScrollBar>
#include <QTextBlock>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QApplication>

EssOutputConsole::EssOutputConsole(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_maxLines(10000)
    , m_showTimestamps(true)
    , m_showSource(false)
    , m_autoScroll(true)
{
    init();
}

EssOutputConsole::~EssOutputConsole()
{
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
}

QSize EssOutputConsole::minimumSizeHint() const
{
    return QSize(200, 50);  // Allow very small height
}

void EssOutputConsole::init()
{
    // Set appearance
    QFont consoleFont("Monaco, Menlo, Courier New");
    consoleFont.setFixedPitch(true);
    consoleFont.setPointSize(9);
    setFont(consoleFont);
    
    // Dark theme
    QPalette p = palette();
    p.setColor(QPalette::Base, QColor(30, 33, 39));        // Very dark gray
    p.setColor(QPalette::Text, QColor(200, 200, 200));     // Light gray
    p.setColor(QPalette::Highlight, QColor(42, 130, 218)); // Blue selection
    p.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(p);
    
    // Read-only console
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMaximumBlockCount(m_maxLines);
    
    // Initialize filters - all enabled by default
    m_typeFilters[OutputType::Info] = true;
    m_typeFilters[OutputType::Success] = true;
    m_typeFilters[OutputType::Warning] = true;
    m_typeFilters[OutputType::Error] = true;
    m_typeFilters[OutputType::Debug] = true;
    m_typeFilters[OutputType::System] = true;
    
    // Update timer for batching
    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(50); // 20 FPS
    connect(m_updateTimer, &QTimer::timeout, this, &EssOutputConsole::processPendingMessages);
}

void EssOutputConsole::logInfo(const QString &message, const QString &source)
{
    log(OutputType::Info, message, source);
}

void EssOutputConsole::logSuccess(const QString &message, const QString &source)
{
    log(OutputType::Success, message, source);
}

void EssOutputConsole::logWarning(const QString &message, const QString &source)
{
    log(OutputType::Warning, message, source);
}

void EssOutputConsole::logError(const QString &message, const QString &source)
{
    log(OutputType::Error, message, source);
}

void EssOutputConsole::logDebug(const QString &message, const QString &source)
{
    log(OutputType::Debug, message, source);
}

void EssOutputConsole::logSystem(const QString &message, const QString &source)
{
    log(OutputType::System, message, source);
}

void EssOutputConsole::log(OutputType type, const QString &message, const QString &source)
{
    // Safety check - don't process if we're being destroyed or app is shutting down
    if (!QCoreApplication::instance()) {
        return;
    }
    
    // Check if our timer is still valid
    if (!m_updateTimer) {
        return;
    }
    
    OutputMessage msg;
    msg.timestamp = QDateTime::currentDateTime();
    msg.type = type;
    msg.source = source;
    msg.message = message;
    
    // Store in history
    m_allMessages.append(msg);
    
    // Queue for display
    m_pendingMessages.enqueue(msg);
    
    // Start update timer if not running
    if (!m_updateTimer->isActive()) {
        m_updateTimer->start();
    }
    
    emit messageLogged(msg);
}

void EssOutputConsole::processPendingMessages()
{
    if (m_pendingMessages.isEmpty()) return;
    
    // Process all pending messages
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    
    while (!m_pendingMessages.isEmpty()) {
        const OutputMessage &msg = m_pendingMessages.dequeue();
        
        // Check filters
        if (!m_typeFilters.value(msg.type, true)) continue;
        if (!msg.source.isEmpty() && m_sourceFilters.contains(msg.source)) continue;
        
        appendMessage(msg);
    }
    
    // Auto scroll if enabled
    if (m_autoScroll) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

void EssOutputConsole::appendMessage(const OutputMessage &message)
{
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    
    // Get format for this message type
    QTextCharFormat format = getFormatForType(message.type);
    
    // Format the message
    QString formatted = formatMessage(message);
    
    cursor.insertText(formatted, format);
    cursor.insertText("\n");
}

QString EssOutputConsole::formatMessage(const OutputMessage &message) const
{
    QString result;
    
    // Timestamp
    if (m_showTimestamps) {
        result += QString("[%1] ").arg(message.timestamp.toString("hh:mm:ss.zzz"));
    }
    
    // Type prefix
    result += getTypePrefix(message.type);
    
    // Source
    if (m_showSource && !message.source.isEmpty()) {
        result += QString("[%1] ").arg(message.source);
    }
    
    // Message
    result += message.message;
    
    return result;
}

QTextCharFormat EssOutputConsole::getFormatForType(OutputType type) const
{
    QTextCharFormat format;
    format.setForeground(getColorForType(type));
    
    if (type == OutputType::Error) {
        format.setFontWeight(QFont::Bold);
    }
    
    return format;
}

QString EssOutputConsole::getTypePrefix(OutputType type) const
{
    switch (type) {
        case OutputType::Info:    return "[INFO] ";
        case OutputType::Success: return "[OK] ";
        case OutputType::Warning: return "[WARN] ";
        case OutputType::Error:   return "[ERROR] ";
        case OutputType::Debug:   return "[DEBUG] ";
        case OutputType::System:  return "[SYS] ";
    }
    return "";
}

QColor EssOutputConsole::getColorForType(OutputType type) const
{
    switch (type) {
        case OutputType::Info:    return QColor(200, 200, 200);  // Light gray
        case OutputType::Success: return QColor(87, 199, 135);   // Green
        case OutputType::Warning: return QColor(255, 195, 0);    // Yellow/Orange
        case OutputType::Error:   return QColor(255, 85, 85);    // Red
        case OutputType::Debug:   return QColor(120, 120, 120);  // Dark gray
        case OutputType::System:  return QColor(86, 182, 255);   // Blue
    }
    return Qt::white;
}

void EssOutputConsole::setMaximumLines(int lines)
{
    m_maxLines = lines;
    setMaximumBlockCount(lines);
}

void EssOutputConsole::setShowTimestamps(bool show)
{
    m_showTimestamps = show;
    updateDisplay();
}

void EssOutputConsole::setShowSource(bool show)
{
    m_showSource = show;
    updateDisplay();
}

void EssOutputConsole::setWordWrap(bool wrap)
{
    setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

void EssOutputConsole::setAutoScroll(bool scroll)
{
    m_autoScroll = scroll;
}

void EssOutputConsole::setTypeFilter(OutputType type, bool enabled)
{
    m_typeFilters[type] = enabled;
    updateDisplay();
}

void EssOutputConsole::setSourceFilter(const QString &source, bool enabled)
{
    if (enabled) {
        m_sourceFilters.remove(source);
    } else {
        m_sourceFilters.insert(source);
    }
    updateDisplay();
}

void EssOutputConsole::clearFilters()
{
    for (auto &filter : m_typeFilters) {
        filter = true;
    }
    m_sourceFilters.clear();
    updateDisplay();
}

void EssOutputConsole::clearConsole()
{
    clear();
    m_allMessages.clear();
}

void EssOutputConsole::updateDisplay()
{
    // Re-display all messages with current filters
    clear();
    
    for (const auto &msg : m_allMessages) {
        // Check filters
        if (!m_typeFilters.value(msg.type, true)) continue;
        if (!msg.source.isEmpty() && m_sourceFilters.contains(msg.source)) continue;
        
        appendMessage(msg);
    }
}

bool EssOutputConsole::saveToFile(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    
    QTextStream stream(&file);
    
    for (const auto &msg : m_allMessages) {
        stream << formatMessage(msg) << Qt::endl;
    }
    
    return true;
}

void EssOutputConsole::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu();
    
    menu->addSeparator();
    
    // Clear action
    QAction *clearAction = menu->addAction(tr("Clear Console"));
    connect(clearAction, &QAction::triggered, this, &EssOutputConsole::clearConsole);
    
    // Save action
    QAction *saveAction = menu->addAction(tr("Save to File..."));
    connect(saveAction, &QAction::triggered, this, [this]() {
        QString filename = QFileDialog::getSaveFileName(this, tr("Save Console Output"),
                                                       "console_output.txt",
                                                       tr("Text Files (*.txt);;All Files (*)"));
        if (!filename.isEmpty()) {
            saveToFile(filename);
        }
    });
    
    menu->addSeparator();
    
    // Filtering submenu
    QMenu *filterMenu = menu->addMenu(tr("Filter Messages"));
    
    // Type filters
    auto addTypeFilter = [this, filterMenu](OutputType type, const QString &name) {
        QAction *action = filterMenu->addAction(name);
        action->setCheckable(true);
        action->setChecked(m_typeFilters.value(type, true));
        connect(action, &QAction::toggled, [this, type](bool checked) {
            setTypeFilter(type, checked);
        });
    };
    
    addTypeFilter(OutputType::Info, tr("Show Info"));
    addTypeFilter(OutputType::Success, tr("Show Success"));
    addTypeFilter(OutputType::Warning, tr("Show Warnings"));
    addTypeFilter(OutputType::Error, tr("Show Errors"));
    addTypeFilter(OutputType::Debug, tr("Show Debug"));
    addTypeFilter(OutputType::System, tr("Show System"));
    
    filterMenu->addSeparator();
    QAction *clearFiltersAction = filterMenu->addAction(tr("Clear All Filters"));
    connect(clearFiltersAction, &QAction::triggered, this, &EssOutputConsole::clearFilters);
    
    // Options submenu
    menu->addSeparator();
    QMenu *optionsMenu = menu->addMenu(tr("Options"));
    
    QAction *timestampAction = optionsMenu->addAction(tr("Show Timestamps"));
    timestampAction->setCheckable(true);
    timestampAction->setChecked(m_showTimestamps);
    connect(timestampAction, &QAction::toggled, this, &EssOutputConsole::setShowTimestamps);
    
    QAction *sourceAction = optionsMenu->addAction(tr("Show Source"));
    sourceAction->setCheckable(true);
    sourceAction->setChecked(m_showSource);
    connect(sourceAction, &QAction::toggled, this, &EssOutputConsole::setShowSource);
    
    QAction *wrapAction = optionsMenu->addAction(tr("Word Wrap"));
    wrapAction->setCheckable(true);
    wrapAction->setChecked(lineWrapMode() != QPlainTextEdit::NoWrap);
    connect(wrapAction, &QAction::toggled, this, &EssOutputConsole::setWordWrap);
    
    QAction *autoScrollAction = optionsMenu->addAction(tr("Auto-Scroll"));
    autoScrollAction->setCheckable(true);
    autoScrollAction->setChecked(m_autoScroll);
    connect(autoScrollAction, &QAction::toggled, this, &EssOutputConsole::setAutoScroll);
    
    menu->exec(event->globalPos());
    delete menu;
}

// Console Manager Implementation
EssConsoleManager* EssConsoleManager::s_instance = nullptr;

EssConsoleManager::EssConsoleManager()
{
}

EssConsoleManager* EssConsoleManager::instance()
{
    if (!s_instance) {
        s_instance = new EssConsoleManager();
    }
    return s_instance;
}

void EssConsoleManager::registerConsole(const QString &name, EssOutputConsole *console)
{
    m_consoles[name] = console;
}

void EssConsoleManager::unregisterConsole(const QString &name)
{
    m_consoles.remove(name);
}

void EssConsoleManager::logInfo(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logInfo(message, source);
    }
}

void EssConsoleManager::logSuccess(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logSuccess(message, source);
    }
}

void EssConsoleManager::logWarning(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logWarning(message, source);
    }
}

void EssConsoleManager::logError(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logError(message, source);
    }
}

void EssConsoleManager::logDebug(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logDebug(message, source);
    }
}

void EssConsoleManager::logSystem(const QString &message, const QString &source)
{
    for (auto console : m_consoles) {
        if (console) console->logSystem(message, source);
    }
}

void EssConsoleManager::logToConsole(const QString &consoleName, OutputType type, 
                                    const QString &message, const QString &source)
{
    if (m_consoles.contains(consoleName) && m_consoles[consoleName]) {
        m_consoles[consoleName]->log(type, message, source);
    }
}

#include "EssWidgetConsole.h"
#include "EssScriptableWidget.h"

#include <QScrollBar>
#include <QMenu>
#include <QPalette>
#include <QApplication>
#include <QClipboard>
#include <QTextCursor>
#include <QDateTime>

EssWidgetConsole::EssWidgetConsole(EssScriptableWidget* parentWidget, QWidget *parent)
    : QTextEdit(parent)
    , m_parentWidget(parentWidget)
{
    init();
}

QSize EssWidgetConsole::minimumSizeHint() const
{
    return QSize(200, 60);
}

QSize EssWidgetConsole::sizeHint() const
{
    return QSize(400, 100);
}

void EssWidgetConsole::init()
{
    // Set read-only
    setReadOnly(true);
    
    // Set compact appearance
    QFont consoleFont("Monaco, Menlo, Courier New");
    consoleFont.setFixedPitch(true);
    consoleFont.setPointSize(9);
    setFont(consoleFont);
    
    // Light console theme
    QPalette p = palette();
    p.setColor(QPalette::Base, QColor(253, 253, 253));      // Very light background
    p.setColor(QPalette::Text, QColor(33, 37, 41));         // Dark text
    p.setColor(QPalette::Highlight, QColor(0, 123, 255));   // Blue selection
    p.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(p);
    
    // Console behavior
    setLineWrapMode(QTextEdit::WidgetWidth);
    setUndoRedoEnabled(false);
    document()->setMaximumBlockCount(500);
    
    // Welcome message
    if (m_parentWidget) {
        QString widgetName = m_parentWidget->name();
        logMessage(QString("=== %1 Console ===").arg(widgetName), OutputType::System);
        logMessage("Widget log messages will appear here", OutputType::Info);
    }
}

void EssWidgetConsole::logMessage(const QString &message, OutputType type)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    appendMessage(logLine + "\n", type);
}

void EssWidgetConsole::clearConsole()
{
    clear();
    if (m_parentWidget) {
        QString widgetName = m_parentWidget->name();
        logMessage(QString("=== %1 Console (cleared) ===").arg(widgetName), OutputType::System);
    }
}

void EssWidgetConsole::appendMessage(const QString &text, OutputType type)
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
            format.setFontWeight(QFont::Bold);
            break;
        case OutputType::Info:
        default:
            format.setForeground(QColor(73, 80, 87));    // Darker gray
            break;
    }
    
    textCursor().insertText(text, format);
    
    // Auto-scroll to bottom
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void EssWidgetConsole::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);
    
    // Add copy action
    QAction *copyAction = menu->addAction(tr("Copy"));
    copyAction->setEnabled(textCursor().hasSelection());
    connect(copyAction, &QAction::triggered, [this]() {
        QTextCursor cursor = textCursor();
        if (cursor.hasSelection()) {
            QString selectedText = cursor.selectedText();
            selectedText.replace(QChar::ParagraphSeparator, '\n');
            QApplication::clipboard()->setText(selectedText);
        }
    });
    
    // Add select all action
    QAction *selectAllAction = menu->addAction(tr("Select All"));
    connect(selectAllAction, &QAction::triggered, this, &QTextEdit::selectAll);
    
    menu->addSeparator();
    
    // Add clear action
    QAction *clearAction = menu->addAction(tr("Clear Console"));
    connect(clearAction, &QAction::triggered, this, &EssWidgetConsole::clearConsole);
    
    // Widget-specific actions
    if (m_parentWidget) {
        menu->addSeparator();
        
        QAction *widgetInfoAction = menu->addAction(tr("Widget Info"));
        connect(widgetInfoAction, &QAction::triggered, [this]() {
            logMessage(QString("Widget: %1").arg(m_parentWidget->name()), OutputType::Info);
            logMessage(QString("Type: %1").arg(m_parentWidget->getWidgetTypeName()), OutputType::Info);
            logMessage(QString("Development mode: %1").arg(m_parentWidget->isDevelopmentMode() ? "enabled" : "disabled"), OutputType::Info);
        });
    }
    
    menu->exec(event->globalPos());
    delete menu;
}
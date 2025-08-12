#pragma once

#include <QPlainTextEdit>
#include <QStringList>
#include <memory>
#include "console/EssOutputConsole.h"

class CommandHistory;
class EssScriptableWidget;

/**
 * @brief Lightweight terminal widget for individual scriptable widgets
 * 
 * Provides a dedicated terminal for each scriptable widget with:
 * - Command execution in the widget's Tcl interpreter
 * - Local command history
 * - Output display with syntax highlighting
 * - Integration with widget's logging
 */
class EssWidgetTerminal : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit EssWidgetTerminal(EssScriptableWidget* parentWidget, QWidget *parent = nullptr);
    ~EssWidgetTerminal();
    
    // Size hints for embedding
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    // Execute command programmatically
    void executeCommand(const QString &command);
    
    // Terminal configuration
    void setPrompt(const QString &prompt);
    QString prompt() const { return m_prompt; }
    
    // Clear terminal
    void clearTerminal();
    
    // Log output from the widget
    void logMessage(const QString &message, OutputType type = OutputType::Info);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void processCommand();
    void onWidgetScriptExecuted(int result, const QString& output);

private:
    void init();
    void setupConnections();
    void appendOutput(const QString &text, OutputType type = OutputType::Info);
    void ensureCursorInEditableArea();
    QString getCurrentCommand() const;
    void replaceCurrentCommand(const QString &newCommand);
    void navigateHistory(int direction);
    void updatePrompt();
    
    // Clipboard operations
    void handleCopy();
    void handlePaste();
    
private:
    EssScriptableWidget* m_parentWidget;
    QString m_prompt;
    int m_promptPosition;
    bool m_isExecutingCommand;
    
    std::unique_ptr<CommandHistory> m_history;
};
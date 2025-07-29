#pragma once

#include <QPlainTextEdit>
#include <QStringList>
#include <memory>
#include "console/EssOutputConsole.h"  // Get OutputType from here

class CommandHistory;
class QCompleter;

class EssTerminalWidget : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit EssTerminalWidget(QWidget *parent = nullptr);
    ~EssTerminalWidget();
    
    // Execute command programmatically
    void executeCommand(const QString &command);
    
    // Terminal configuration
    void setPrompt(const QString &prompt);
    QString prompt() const { return m_prompt; }
    
    // Clear terminal
    void clearTerminal();

signals:
    // Optional: emit status messages for status bar
    void statusMessage(const QString &message, int timeout = 0);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void processCommand();
    void updatePrompt(const QString &newPrompt = QString());
    void insertCompletion(const QString &completion);

private:
    void init();
    void setupCommandInterface();
    void setupCompleter();
    void updateCompletionList();
    void appendOutput(const QString &text, OutputType type = OutputType::Info);
    void ensureCursorInEditableArea();
    QString getCurrentCommand() const;
    void replaceCurrentCommand(const QString &newCommand);
    void navigateHistory(int direction);
    
    QString m_prompt;
    int m_promptPosition;
    bool m_isExecutingCommand;
    int m_currentLineStart;
    
    std::unique_ptr<CommandHistory> m_history;
    QCompleter *m_completer;
};

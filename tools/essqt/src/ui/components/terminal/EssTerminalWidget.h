#pragma once

#include <QPlainTextEdit>
#include <QStringList>
#include <memory>
#include "console/EssOutputConsole.h"  // Get OutputType from here

class CommandHistory;
class EssCommandInterface;
class QCompleter;

class EssTerminalWidget : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit EssTerminalWidget(QWidget *parent = nullptr);
    ~EssTerminalWidget();
    
    // Connection management
    bool connectToHost(const QString &host);
    void disconnectFromHost();
    bool isConnected() const;
    
    // Execute command programmatically
    void executeCommand(const QString &command);
    
    // Terminal configuration
    void setPrompt(const QString &prompt);
    QString prompt() const { return m_prompt; }
    
    // Clear terminal
    void clearTerminal();

signals:
    void commandExecuted(const QString &command);
    void commandResult(const QString &result, bool isError);

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
    void showHelp();
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
    std::unique_ptr<EssCommandInterface> m_commandInterface;
    QCompleter *m_completer;
};

#pragma once
#include <QWidget>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QTextEdit>
#include <QStringList>
#include "TclInterp.h"

class TclConsoleWidget : public QWidget {
    Q_OBJECT

public:
    explicit TclConsoleWidget(QWidget *parent = nullptr);
    ~TclConsoleWidget();
    
    // Public interface for external Tcl command execution
    QString evaluateCommand(const QString& command);
    bool evaluateCommandWithResult(const QString& command, QString& result);
    
    // Data integration methods
    void putDynGroup(DYN_GROUP* dg);
    DYN_LIST* findDynList(DYN_GROUP* dg, const QString& name);
    
signals:
    void commandExecuted(const QString& command, const QString& result);
    void errorOccurred(const QString& error);

private slots:
    void executeCommand();
    void clearConsole();
    void loadScript();
    void saveScript();

private:
    void setupUI();
    void setupTclEnvironment();
    void appendOutput(const QString& text, bool isError = false);
    
    // UI Components
    QSplitter* splitter;
    QPlainTextEdit* outputArea;
    QTextEdit* scriptArea;
    QLineEdit* commandLine;
    QPushButton* executeButton;
    QPushButton* clearButton;
    QPushButton* loadScriptButton;
    QPushButton* saveScriptButton;
    QLabel* statusLabel;
    
    // Tcl Integration
    TclInterp* tclInterp;
    
    // Command history
    QStringList commandHistory;
    int historyIndex;
};

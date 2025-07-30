#pragma once

#include <QWidget>
#include <QString>
#include <memory>

class QsciScintilla;
class QsciLexer;
class QAction;
class QToolBar;
class QLabel;

class EssScriptEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssScriptEditorWidget(QWidget *parent = nullptr);
    ~EssScriptEditorWidget();

    // File operations
    void newFile();
    void openFile(const QString &path = QString());
    bool saveFile();
    bool saveFileAs();
    
    // Editor operations
    void executeSelection();
    void executeAll();
    
    QString currentFile() const { return m_currentFile; }
    bool isModified() const;

signals:
    void fileOpened(const QString &path);
    void fileSaved(const QString &path);
    void executeRequested(const QString &code);
    void modificationChanged(bool modified);
    void statusMessage(const QString &message, int timeout = 0);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onTextChanged();
    void onCursorPositionChanged(int line, int index);
    void onModificationChanged(bool modified);
    
private:
    void setupEditor();
    void setupActions();
    void createToolBar();
    void setCurrentFile(const QString &path);
    bool maybeSave();
    void applyTheme();
    void loadFile(const QString &path);
    void setLexerForFile(const QString &path);

    void setupEmacsBindings();
    void smartIndent();
    bool eventFilter(QObject *obj, QEvent *event) override;
  
    QsciScintilla *m_editor;
    std::unique_ptr<QsciLexer> m_lexer;
    QToolBar *m_toolbar;
    QLabel *m_statusLabel;
    
    QString m_currentFile;
    QString m_defaultPath;
    
    // Actions
    QAction *m_newAction;
    QAction *m_openAction;
    QAction *m_saveAction;
    QAction *m_saveAsAction;
    QAction *m_executeSelAction;
    QAction *m_executeAllAction;
    QAction *m_findAction;
};

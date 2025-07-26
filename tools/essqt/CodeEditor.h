// CodeEditor.h
#pragma once
#include <QPlainTextEdit>

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr);

 protected:
    void keyPressEvent(QKeyEvent* event) override;
    
signals:
    void sendText(const QString& text);  // Emitted when user wants to send content
};

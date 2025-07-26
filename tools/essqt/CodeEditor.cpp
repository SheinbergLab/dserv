// CodeEditor.cpp
#include "CodeEditor.h"
#include <QKeyEvent>

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent) {}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
#if defined(Q_OS_MAC)
  const auto triggerModifier = Qt::MetaModifier;
#else
  const auto triggerModifier = Qt::ControlModifier;
#endif
  
  if ((event->modifiers() & triggerModifier) && event->key() == Qt::Key_Return) {
    emit sendText(toPlainText());
  } else {
    QPlainTextEdit::keyPressEvent(event);
  }
}

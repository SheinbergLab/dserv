#pragma once

#include <QTextEdit>
#include "console/EssOutputConsole.h"

class EssScriptableWidget;

/**
 * @brief Simple console widget for individual scriptable widgets
 * 
 * Provides a dedicated log console for each scriptable widget with:
 * - Read-only log display
 * - Color-coded message types
 * - Auto-scrolling
 * - Compact size for development panels
 */
class EssWidgetConsole : public QTextEdit
{
    Q_OBJECT

public:
    explicit EssWidgetConsole(EssScriptableWidget* parentWidget, QWidget *parent = nullptr);
    ~EssWidgetConsole() = default;
    
    // Size hints for embedding
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
    
    // Log message to console
    void logMessage(const QString &message, OutputType type = OutputType::Info);
    
    // Clear console
    void clearConsole();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void init();
    void appendMessage(const QString &text, OutputType type);
    
private:
    EssScriptableWidget* m_parentWidget;
};
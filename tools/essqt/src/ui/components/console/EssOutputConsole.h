#pragma once

#include <QPlainTextEdit>
#include <QDateTime>
#include <QQueue>

class QMenu;
class QTimer;

// Output message types
enum class OutputType {
    Info,
    Success,
    Warning,
    Error,
    Debug,
    System
};

// Single output message
struct OutputMessage {
    QDateTime timestamp;
    OutputType type;
    QString source;
    QString message;
};

class EssOutputConsole : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit EssOutputConsole(QWidget *parent = nullptr);
    ~EssOutputConsole();
    
    // size hint
	QSize minimumSizeHint() const override;
	
    // Logging methods
    void logInfo(const QString &message, const QString &source = QString());
    void logSuccess(const QString &message, const QString &source = QString());
    void logWarning(const QString &message, const QString &source = QString());
    void logError(const QString &message, const QString &source = QString());
    void logDebug(const QString &message, const QString &source = QString());
    void logSystem(const QString &message, const QString &source = QString());
    
    // Generic log method
    void log(OutputType type, const QString &message, const QString &source = QString());
    
    // Configuration
    void setMaximumLines(int lines);
    void setShowTimestamps(bool show);
    void setShowSource(bool show);
    void setWordWrap(bool wrap);
    void setAutoScroll(bool scroll);
    
    // Filtering
    void setTypeFilter(OutputType type, bool enabled);
    void setSourceFilter(const QString &source, bool enabled);
    void clearFilters();
    
    // Clear console
    void clearConsole();
    
    // Save to file
    bool saveToFile(const QString &filename);

signals:
    void messageLogged(const OutputMessage &message);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    
private slots:
    void processPendingMessages();
    void updateDisplay();

private:
    void init();
    void appendMessage(const OutputMessage &message);
    QString formatMessage(const OutputMessage &message) const;
    QTextCharFormat getFormatForType(OutputType type) const;
    QString getTypePrefix(OutputType type) const;
    QColor getColorForType(OutputType type) const;
    
    // Configuration
    int m_maxLines;
    bool m_showTimestamps;
    bool m_showSource;
    bool m_autoScroll;
    
    // Copy text
    void handleCopy();
    
    // Filtering
    QHash<OutputType, bool> m_typeFilters;
    QSet<QString> m_sourceFilters;
    
    // Message queue for batching
    QQueue<OutputMessage> m_pendingMessages;
    QTimer *m_updateTimer;
    
    // All messages (for filtering/saving)
    QList<OutputMessage> m_allMessages;
};

// Singleton console manager for global access
class EssConsoleManager : public QObject
{
    Q_OBJECT
    
public:
    static EssConsoleManager* instance();
    
    void registerConsole(const QString &name, EssOutputConsole *console);
    void unregisterConsole(const QString &name);
    
    // Log to all registered consoles
    void logInfo(const QString &message, const QString &source = QString());
    void logSuccess(const QString &message, const QString &source = QString());
    void logWarning(const QString &message, const QString &source = QString());
    void logError(const QString &message, const QString &source = QString());
    void logDebug(const QString &message, const QString &source = QString());
    void logSystem(const QString &message, const QString &source = QString());
    
    // Log to specific console
    void logToConsole(const QString &consoleName, OutputType type, 
                      const QString &message, const QString &source = QString());
    
private:
    EssConsoleManager();
    static EssConsoleManager *s_instance;
    QHash<QString, EssOutputConsole*> m_consoles;
};

// Convenience macros for logging
#define ESS_LOG_INFO(msg) EssConsoleManager::instance()->logInfo(msg, __FUNCTION__)
#define ESS_LOG_SUCCESS(msg) EssConsoleManager::instance()->logSuccess(msg, __FUNCTION__)
#define ESS_LOG_WARNING(msg) EssConsoleManager::instance()->logWarning(msg, __FUNCTION__)
#define ESS_LOG_ERROR(msg) EssConsoleManager::instance()->logError(msg, __FUNCTION__)
#define ESS_LOG_DEBUG(msg) EssConsoleManager::instance()->logDebug(msg, __FUNCTION__)

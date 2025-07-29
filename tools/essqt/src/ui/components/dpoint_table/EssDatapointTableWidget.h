// EssDatapointTableWidget.h
#pragma once

#include <QDockWidget>
#include <QVariant>
#include <QString>
#include <QRegularExpression>
#include <memory>

class QTableWidget;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QSpinBox;

class EssDatapointTableWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssDatapointTableWidget(QWidget *parent = nullptr);
    ~EssDatapointTableWidget();

    // Configuration
    void setMaxRows(int maxRows);
    int maxRows() const { return m_maxRows; }
    
    // Control
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    
    // Filtering
    void setFilterPattern(const QString &pattern);
    QString filterPattern() const;
    
    // Clear the table
    void clearTable();

private slots:
    // Data processor signal
    void onGenericDatapointReceived(const QString &name, const QVariant &value, qint64 timestamp);
    
    // UI controls
    void onPauseToggled(bool checked);
    void onClearClicked();
    void onFilterChanged(const QString &text);
    void onMaxRowsChanged(int value);
    void onAutoScrollToggled(bool checked);
    
    // Connection management
    void onHostDisconnected();

private:
    void setupUi();
    void connectSignals();
    void addDatapointRow(const QString &name, const QVariant &value, qint64 timestamp);
    QString formatTimestamp(qint64 timestamp) const;
    QString formatValue(const QVariant &value) const;
    QString getDataTypeString(const QVariant &value) const;
    bool matchesFilter(const QString &name) const;
    void trimTableRows();
    void applyFilterToExistingRows();
    
    // UI elements
    QTableWidget *m_tableWidget;
    QLineEdit *m_filterEdit;
    QPushButton *m_pauseButton;
    QPushButton *m_clearButton;
    QCheckBox *m_autoScrollCheck;
    QSpinBox *m_maxRowsSpinBox;
    
    // State
    bool m_paused;
    bool m_autoScroll;
    int m_maxRows;
    QRegularExpression m_filterRegex;
    
    // Performance optimization
    int m_updateCounter;
    static constexpr int BATCH_UPDATE_SIZE = 10;
};

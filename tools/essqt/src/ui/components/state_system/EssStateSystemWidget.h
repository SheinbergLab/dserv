// EssStateSystemWidget.h - Unified trace view with debug features
#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <memory>

#include "EssStateDebugData.h"

class EssEventProcessor;
class EssDataProcessor;
class EssEvent;

/**
 * @brief State system widget with trace view
 * 
 * This widget provides a trace table view that shows:
 * - State transitions with timing information
 * - Debug events when backend debug is enabled
 * - Navigation through historical observations
 */
class EssStateSystemWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssStateSystemWidget(QWidget *parent = nullptr);
    ~EssStateSystemWidget();

public slots:
    /**
     * @brief Clear all state information
     */
    void clear();

private slots:
    void onSystemStateChanged(int state);
    void onExperimentStateChanged(const QString &newState);
    void onDatapointUpdate(const QString &name, const QVariant &value, qint64 timestamp);
    void onEventReceived(const EssEvent &event);
    void onHostConnected(const QString &host);
    void onHostDisconnected();
    void onRefreshClicked();
    void onBackendDebugToggled(bool enabled);
    void onObservationStarted(uint64_t timestamp);
    void onObservationEnded();
    
    // Observation navigation
    void onPrevObservation();
    void onNextObservation();
    void onObservationChanged(int obsIndex);

private:
    // UI Components - Header
    QLabel *m_statusLabel;
    QPushButton *m_refreshButton;
    QCheckBox *m_backendDebugCheckbox;
    
    // UI Components - Main view
    QTableWidget *m_traceTable;
    
    // UI Components - Navigation
    QWidget *m_obsNavigationPanel;
    QPushButton *m_prevObsButton;
    QPushButton *m_nextObsButton;
    QSpinBox *m_obsSpinBox;
    QLabel *m_obsInfoLabel;

    // Data
    QStringList m_allStates;
    QString m_currentState;
    bool m_systemRunning;
    bool m_connected;
    bool m_backendDebugEnabled; // Backend debug state
    std::unique_ptr<StateDebugSession> m_debugSession;
    int m_viewingObsIndex; // -1 = current/live, 0+ = historical observation

    // Connections
    EssEventProcessor *m_eventProcessor;
    EssDataProcessor *m_dataProcessor;

    // Methods
    void setupUi();
    void connectToDataProcessor();
    void updateStatusLabel();
    void updateTraceTable();
    void setCurrentState(const QString &stateName);
    void loadStateTable(const QString &stateTableStr);

    // Debug Methods
    void processDebugEvent(const EssEvent &event);
    void enableBackendDebug(bool enable);
    StateDebugEvent parseDebugEvent(const EssEvent &event);
    void updateObservationNavigation();
    void showObservation(int obsIndex);
    
    // Helper Methods
    QString formatDuration(qint64 milliseconds) const;
};
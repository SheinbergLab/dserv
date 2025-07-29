// EssEventTableWidget.h
#pragma once

#include <QWidget>
#include <cstdint>
#include "core/EssEvent.h"  // For SystemState enum

class QTableWidget;
class QPushButton;
class QLabel;
class EssEventProcessor;

class EssEventTableWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssEventTableWidget(QWidget *parent = nullptr);
    ~EssEventTableWidget();

private slots:
    void onEventReceived(const EssEvent &event);
    void onSystemStateChanged(SystemState state);
    void onObservationStarted(uint64_t timestamp);
    void onObservationReset();
    void onClearClicked();
    void onHostConnected(const QString &host);
    void onHostDisconnected();

private:
    void setupUi();
    void connectToEventProcessor();
    void addEventRow(const EssEvent &event);
    void clearEvents();
    QString formatEventParams(const EssEvent &event) const;
    bool shouldDisplayEvent(const EssEvent &event) const;  // New method for filtering

    QTableWidget *m_tableWidget;
    QPushButton *m_clearButton;
    QLabel *m_statusLabel;
    QLabel *m_obsLabel;
    
    int m_maxEvents;
    uint64_t m_currentObsStart;
    EssEventProcessor *m_eventProcessor;
};
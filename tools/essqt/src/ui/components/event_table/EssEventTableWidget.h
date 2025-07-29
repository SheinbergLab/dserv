// EssEventTableWidget.h
#pragma once

#include <QWidget>  // Change from QDockWidget
#include <QTableWidget>
#include "core/EssEvent.h"

class QPushButton;
class QLabel;
class EssEventProcessor;

class EssEventTableWidget : public QWidget  // Change from QDockWidget
{
    Q_OBJECT

public:
    explicit EssEventTableWidget(QWidget *parent = nullptr);
    ~EssEventTableWidget();

    // Clear the event table
    void clearEvents();
    
    // Set max events to display
    void setMaxEvents(int max) { m_maxEvents = max; }

private slots:
    void onEventReceived(const EssEvent &event);
    void onSystemStateChanged(SystemState state);
    void onObservationStarted(uint64_t timestamp);
    void onObservationReset();
    void onClearClicked();

    // Connection management
	void onHostConnected(const QString &host); 
    void onHostDisconnected();
    
private:
    void setupUi();
    void connectToEventProcessor();
    void addEventRow(const EssEvent &event);
    QString formatEventParams(const EssEvent &event) const;
    
    QTableWidget *m_tableWidget;
    QPushButton *m_clearButton;
    QLabel *m_statusLabel;
    QLabel *m_obsLabel;
    
    int m_maxEvents;
    uint64_t m_currentObsStart;
    EssEventProcessor *m_eventProcessor;  // This was missing
};

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
    void onExperimentStateChanged(const QString &newstate);
    void onObservationStarted(uint64_t timestamp);
    void onObservationReset();
    void onClearClicked();
    void onHostConnected(const QString &host);
    void onHostDisconnected();

private:
  struct ObservationData {
    uint64_t startTime;
    int obsCount;
    int obsTotal;
    QList<EssEvent> events;
  };
  
  void setupUi();
  
  // Add these member variables
  QList<ObservationData> m_observationHistory;
  int m_currentObsIndex;
  
  // Add method for navigation
  void showObservation(int index);
  void updateNavigationControls();
  
  // Add UI elements for navigation
  QPushButton *m_prevObsButton;
  QPushButton *m_nextObsButton;
  QLabel *m_obsNavigationLabel;
  
  void connectToEventProcessor();
  void addEventRow(const EssEvent &event);
  void clearEvents();
  QString formatEventParams(const EssEvent &event) const;
  bool shouldDisplayEvent(const EssEvent &event) const;  // New method for filtering
  void extractObservationParams(const EssEvent &event);
  void updateObservationLabel();
  QTableWidget *m_tableWidget;
  QPushButton *m_clearButton;
  QLabel *m_statusLabel;
  QLabel *m_obsLabel;
  int m_obsCount;
  int m_obsTotal;
  int m_maxEvents;
  uint64_t m_currentObsStart;
  EssEventProcessor *m_eventProcessor;
};

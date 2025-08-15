// core/EssDataProcessor.h
#pragma once

#include <QObject>
#include <QPointF>
#include <QVariant>
#include <QString>

#include "EssEventProcessor.h"

class EssEventProcessor;
struct EssEvent;

class EssDataProcessor : public QObject
{
    Q_OBJECT

public:
    explicit EssDataProcessor(QObject *parent = nullptr);
    ~EssDataProcessor();

    // Process incoming datapoint and route to appropriate signal
    void processDatapoint(const QString &name, const QVariant &value, 
    				      qint64 timestamp, int dtype = -1);

    EssEventProcessor* eventProcessor() const { return m_eventProcessor; }
  
signals:
    // Eye tracking data
    void eyePositionUpdated(const QPointF &position, qint64 timestamp);
    void eyeVelocityUpdated(const QPointF &velocity, qint64 timestamp);
    
    // Experiment events
    void experimentEventReceived(const QString &eventType, const QString &eventData, qint64 timestamp);
    void experimentStateChanged(const QString &state);

	// Datafile    
    void datafileChanged(const QString &filename);
    void datafileProcessed(const QString &filename);

    // System status
    void systemStatusUpdated(const QString &status);
    void systemConnected(const QString &system, const QString &protocol, const QString &variant);
    
    // Parameter updates
    void parameterChanged(const QString &name, const QVariant &value);
    
    // Data tables (DG format)
    void stimulusDataReceived(const QByteArray &dgData, qint64 timestamp);
    void trialDataReceived(const QByteArray &dgData, qint64 timestamp);

    // event log entries
    void eventLogEntryReceived(const EssEvent &event);
    void observationStarted(uint64_t timestamp);
    void observationEnded(uint64_t timestamp);  
    void observationReset();
  
    // Generic datapoint (for anything not specifically handled)
    void genericDatapointReceived(const QString &name, const QVariant &value, qint64 timestamp);

   // DynGroup registered
   void dynGroupRegistered(const QString &name);

private:
    // Parsing helpers
    QPointF parseEyePosition(const QVariant &data);
    void parseExperimentEvent(const QVariant &data, QString &type, QString &eventData);
    
    // Route specific datapoint patterns
    void routeEyeData(const QString &name, const QVariant &value, qint64 timestamp);
    void routeEssData(const QString &name, const QVariant &value, qint64 timestamp);
    void routeDgData(const QString &name, const QVariant &value, qint64 timestamp);

	// DynGroup processing
	bool processDynGroup(const QString &name, const QByteArray &data);

  EssEventProcessor *m_eventProcessor;
};

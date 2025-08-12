// EssEventProcessor.h
#pragma once

#include <QObject>
#include <QMap>
#include <QStringList>
#include "EssEvent.h"

class EssEventProcessor : public QObject
{
    Q_OBJECT

public:
    explicit EssEventProcessor(QObject *parent = nullptr);
    
    // Process an eventlog/events datapoint
    void processEventLogDatapoint(const QVariant &data, qint64 timestamp);
    
    // Process an event directly (made public for evt:TYPE:SUBTYPE handling)
    void processEvent(const EssEvent &event);
    
    // Get event type/subtype names
    QString getEventTypeName(uint8_t type) const;
    QString getEventSubtypeName(uint8_t type, uint8_t subtype) const;
    
    uint8_t getEventTypeId(const QString& name) const;
    QPair<uint8_t, uint8_t> getEventSubtypeId(const QString& name) const;
    QPair<uint8_t, uint8_t> getEventSubtypeId(uint8_t type, const QString& subtypeName) const;
    
    bool isValidEventTypeName(const QString& name) const;
    bool isValidEventSubtypeName(const QString& name) const;
    bool isValidEventSubtypeName(uint8_t type, const QString& subtypeName) const;
    
    QStringList getAvailableEventTypeNames() const;
    QStringList getAvailableEventSubtypeNames(uint8_t type) const;


    // Get current system state
    SystemState systemState() const { return m_systemState; }
    
    // Get observation info
    const ObservationInfo& observationInfo() const { return m_obsInfo; }

signals:
    // High-level signals
    void systemStateChanged(SystemState state);
    void observationStarted(uint64_t timestamp);
    void observationEnded(uint64_t timestamp);
    void observationReset();
    
    // Event signals
    void eventReceived(const EssEvent &event);
    void userEventReceived(uint8_t subtype, const QString &params);
    void fileIOEventReceived(const EssEvent &event);
    
    // Name updates
    void eventTypeNameSet(uint8_t type, const QString &name);
    void eventSubtypeNamesSet(uint8_t type, const QMap<uint8_t, QString> &names);

private:
    void initializeDefaultNames();
    
    // Event type names (256 possible types)
    QString m_eventTypeNames[256];
    
    QMap<QString, uint8_t> m_typeNameToId;
    QMap<QString, QPair<uint8_t, uint8_t>> m_subtypeNameToId;
    
    // Event subtype names: key is "type:subtype"
    QMap<QString, QString> m_eventSubtypeNames;
    
    // Current system state
    SystemState m_systemState;
    
    // Observation tracking
    ObservationInfo m_obsInfo;
};

// EssEventProcessor.cpp
#include "EssEventProcessor.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

EssEventProcessor::EssEventProcessor(QObject *parent)
    : QObject(parent)
    , m_systemState(SYSTEM_STOPPED)
{
    initializeDefaultNames();
}

void EssEventProcessor::initializeDefaultNames()
{
    // Initialize default event type names (matching Go code)
    for (int i = 0; i < 16; i++) {
        m_eventTypeNames[i] = QString("Reserved%1").arg(i);
    }
    for (int i = 16; i < 128; i++) {
        m_eventTypeNames[i] = QString("System%1").arg(i);
    }
    for (int i = 128; i < 256; i++) {
        m_eventTypeNames[i] = QString("User%1").arg(i);
    }
}

void EssEventProcessor::processEvent(const EssEvent &event)
{
    switch (event.type) {
        case EVT_USER: // User control events
            switch (event.subtype) {
                case USER_SYSTEM_START:
                    m_systemState = SYSTEM_RUNNING;
                    emit systemStateChanged(m_systemState);
                    break;
                case USER_SYSTEM_STOP:
                    m_systemState = SYSTEM_STOPPED;
                    emit systemStateChanged(m_systemState);
                    break;
                case USER_OBS_RESET:
                    m_obsInfo.reset();
                    emit observationReset();
                    break;
            }
            emit userEventReceived(event.subtype, event.paramsAsString());
            break;
            
        case EVT_FILEIO:
            emit fileIOEventReceived(event);
            break;
            
        case EVT_NAMESET:
            // Set the name for an event type
            // IMPORTANT: event.subtype contains the TYPE to name, not a subtype!
            if (event.params.isString()) {
                QString name = event.params.toString();
                uint8_t typeToName = event.subtype;  // This is the type we're naming
                m_eventTypeNames[typeToName] = name;
                emit eventTypeNameSet(typeToName, name);
            }
            return; // Don't add to observation
            
        case EVT_SUBTYPE_NAMES:
            // Parse space-separated subtype names for a given event type
            // event.subtype contains the event TYPE whose subtypes we're naming
            if (event.params.isString()) {
                QStringList parts = event.params.toString().split(' ');
                QMap<uint8_t, QString> subtypeMap;
                
                // Parts come in pairs: NAME first, then ID
                // Example: "DURATION 0 TYPE 1 MICROLITERS 2"
                for (int i = 0; i < parts.size() - 1; i += 2) {
                    QString subtypeName = parts[i];      // NAME is first
                    uint8_t subtypeId = parts[i + 1].toUInt();  // ID is second
                    
                    // Key format is "type:subtype"
                    QString key = QString("%1:%2").arg(event.subtype).arg(subtypeId);
                    m_eventSubtypeNames[key] = subtypeName;
                    subtypeMap[subtypeId] = subtypeName;
                }
                
                emit eventSubtypeNamesSet(event.subtype, subtypeMap);
            }
            return; // Don't add to observation
            
        case EVT_BEGINOBS:
            m_obsInfo.start(event);
            emit observationStarted(event.timestamp);
            break;
            
        case EVT_ENDOBS:
            emit observationEnded(event.timestamp);
            break;
    }
    
    // Add event to current observation if active
    if (m_obsInfo.isActive()) {
        m_obsInfo.addEvent(event);
    }
    
    // Always emit the generic event signal
    emit eventReceived(event);
}

QString EssEventProcessor::getEventTypeName(uint8_t type) const
{
    return m_eventTypeNames[type];
}

QString EssEventProcessor::getEventSubtypeName(uint8_t type, uint8_t subtype) const
{
    // Check for named subtypes first
    QString key = QString("%1:%2").arg(type).arg(subtype);
    if (m_eventSubtypeNames.contains(key)) {
        return m_eventSubtypeNames[key];
    }
    
    // Special handling for known event types
    switch (type) {
        case EVT_USER: // User events
            switch (subtype) {
                case USER_SYSTEM_START: return "START";
                case USER_SYSTEM_STOP: return "STOP";
                case USER_OBS_RESET: return "RESET";
                default: break;
            }
            break;
            
        case EVT_BEGINOBS:
            return QString("Obs %1").arg(subtype);
            
        case EVT_ENDOBS:
            return QString("Obs %1").arg(subtype);
            
        // Add more special cases as needed
    }
    
    return QString::number(subtype);
}

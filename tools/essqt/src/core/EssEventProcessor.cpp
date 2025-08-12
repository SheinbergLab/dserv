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
    // Your existing code...
    if (event.params.isString()) {
        QString name = event.params.toString();
        uint8_t typeToName = event.subtype;
        
        // Remove old inverse mapping if it exists
        QString oldName = m_eventTypeNames[typeToName];
        if (!oldName.isEmpty() && !oldName.startsWith("Reserved") && 
            !oldName.startsWith("System") && !oldName.startsWith("User")) {
            m_typeNameToId.remove(oldName);
        }
        
        m_eventTypeNames[typeToName] = name;
        
        // ADD THIS LINE: Update inverse mapping
        m_typeNameToId[name] = typeToName;
        
        emit eventTypeNameSet(typeToName, name);
    }
    return;

case EVT_SUBTYPE_NAMES:
    // Your existing code...
    if (event.params.isString()) {
        QStringList parts = event.params.toString().split(' ');
        QMap<uint8_t, QString> subtypeMap;
        
        // ADD THIS: Clear old inverse mappings for this type
        auto it = m_subtypeNameToId.begin();
        while (it != m_subtypeNameToId.end()) {
            if (it.value().first == event.subtype) {
                it = m_subtypeNameToId.erase(it);
            } else {
                ++it;
            }
        }
        
        for (int i = 0; i < parts.size() - 1; i += 2) {
            QString subtypeName = parts[i];
            uint8_t subtypeId = parts[i + 1].toUInt();
            
            QString key = QString("%1:%2").arg(event.subtype).arg(subtypeId);
            m_eventSubtypeNames[key] = subtypeName;
            subtypeMap[subtypeId] = subtypeName;
            
            // ADD THIS LINE: Update inverse mapping
            m_subtypeNameToId[subtypeName] = qMakePair(event.subtype, subtypeId);
        }
        
        emit eventSubtypeNamesSet(event.subtype, subtypeMap);
    }
    return;
                        
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

uint8_t EssEventProcessor::getEventTypeId(const QString& name) const
{
    return m_typeNameToId.value(name, 255);  // 255 as invalid marker
}

QPair<uint8_t, uint8_t> EssEventProcessor::getEventSubtypeId(const QString& name) const
{
    return m_subtypeNameToId.value(name, qMakePair(uint8_t(255), uint8_t(255)));
}

QPair<uint8_t, uint8_t> EssEventProcessor::getEventSubtypeId(uint8_t type, const QString& subtypeName) const
{
    auto it = m_subtypeNameToId.find(subtypeName);
    if (it != m_subtypeNameToId.end() && it.value().first == type) {
        return it.value();
    }
    return qMakePair(uint8_t(255), uint8_t(255));
}

bool EssEventProcessor::isValidEventTypeName(const QString& name) const
{
    return m_typeNameToId.contains(name);
}

bool EssEventProcessor::isValidEventSubtypeName(const QString& name) const
{
    return m_subtypeNameToId.contains(name);
}

bool EssEventProcessor::isValidEventSubtypeName(uint8_t type, const QString& subtypeName) const
{
    auto pair = getEventSubtypeId(type, subtypeName);
    return pair.first != 255 && pair.second != 255;
}

QStringList EssEventProcessor::getAvailableEventTypeNames() const
{
    QStringList names;
    for (auto it = m_typeNameToId.begin(); it != m_typeNameToId.end(); ++it) {
        if (!it.key().startsWith("Reserved") && 
            !it.key().startsWith("System") && 
            !it.key().startsWith("User")) {
            names.append(it.key());
        }
    }
    return names;
}

QStringList EssEventProcessor::getAvailableEventSubtypeNames(uint8_t type) const
{
    QStringList names;
    for (auto it = m_subtypeNameToId.begin(); it != m_subtypeNameToId.end(); ++it) {
        if (it.value().first == type) {
            names.append(it.key());
        }
    }
    return names;
}
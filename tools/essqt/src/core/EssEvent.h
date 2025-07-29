// EssEvent.h
#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>      // Add this
#include <QJsonValue>      // Add this
#include <QVector>         // Add this for QVector
#include <cstdint>

// Event parameter types (matching Go constants)
enum EventParamType {
    PTYPE_BYTE = 0,
    PTYPE_STRING = 1,
    PTYPE_FLOAT = 2,
    PTYPE_SHORT = 4,
    PTYPE_INT = 5
};

// System states
enum SystemState {
    SYSTEM_STOPPED = 0,
    SYSTEM_RUNNING = 1
};

// Special event types
enum SpecialEventType {
    EVT_NAMESET = 1,        // Sets event type name
    EVT_FILEIO = 2,         // File I/O event
    EVT_USER = 3,           // User control events
    EVT_TRACE = 4,
    EVT_PARAM = 5,
    EVT_SUBTYPE_NAMES = 6,  // Sets subtype names
    EVT_SYSTEM_CHANGES = 18,
    EVT_BEGINOBS = 19,      // Begin observation
    EVT_ENDOBS = 20         // End observation
};

// User event subtypes
enum UserEventSubtype {
    USER_SYSTEM_START = 0,
    USER_SYSTEM_STOP = 1,
    USER_OBS_RESET = 2
};

struct EssEvent {
    uint8_t type;
    uint8_t subtype;
    uint64_t timestamp;
    uint8_t ptype;
    QJsonValue params;  // Can be string, number, array, etc.
    
    // Helper to get params as string
    QString paramsAsString() const {
        if (params.isString()) {
            return params.toString();
        } else if (params.isArray() || params.isObject()) {
            return QJsonDocument(params.isArray() ? 
                QJsonDocument(params.toArray()) : 
                QJsonDocument(params.toObject())).toJson(QJsonDocument::Compact);
        }
        return params.toString();
    }
};

class ObservationInfo {
public:
    ObservationInfo() : obsCount(-1), obsStart(0) {}
    
    void reset() {
        obsCount = -1;
        obsStart = 0;
        events.clear();
    }
    
    void start(const EssEvent& e) {
        obsCount++;
        obsStart = e.timestamp;
        events.append(QVector<EssEvent>{e});
    }
    
    void addEvent(EssEvent e) {
        if (obsCount == -1) return;
        e.timestamp -= obsStart;  // Make relative to observation start
        events[obsCount].append(e);
    }
    
    bool isActive() const { return obsCount >= 0; }
    
    int obsCount;
    uint64_t obsStart;
    QVector<QVector<EssEvent>> events;
};

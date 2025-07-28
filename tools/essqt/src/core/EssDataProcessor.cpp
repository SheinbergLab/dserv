#include "EssDataProcessor.h"
#include "console/EssOutputConsole.h"
#include "core/EssEventProcessor.h"  // Add this
#include "core/EssEvent.h"           // Add this
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

// In the constructor:
EssDataProcessor::EssDataProcessor(QObject *parent)
    : QObject(parent)
{
    // Initialize event processor FIRST
    m_eventProcessor = new EssEventProcessor(this);
    
    // THEN connect signals
    connect(m_eventProcessor, &EssEventProcessor::systemStateChanged,
            this, [this](SystemState state) {
                emit experimentStateChanged(state == SYSTEM_RUNNING ? "Running" : "Stopped");
            });
    
    connect(m_eventProcessor, &EssEventProcessor::eventReceived,
            this, [this](const EssEvent &event) {
                emit eventLogEntryReceived(event);
            });
    
    // Forward other signals
    connect(m_eventProcessor, &EssEventProcessor::observationStarted,
            this, &EssDataProcessor::observationStarted);
    connect(m_eventProcessor, &EssEventProcessor::observationEnded,
            this, &EssDataProcessor::observationEnded);
    connect(m_eventProcessor, &EssEventProcessor::observationReset,
            this, &EssDataProcessor::observationReset);
}

EssDataProcessor::~EssDataProcessor()
{
}

// In EssDataProcessor::processDatapoint, add this at the beginning of the method:

void EssDataProcessor::processDatapoint(const QString &name, const QVariant &value, qint64 timestamp)
{
    // Handle eventlog/events which contains event data in a QVariantMap
    if (name == "eventlog/events") {
        
        // For eventlog/events, the value should be a QVariantMap with event fields
        if (value.userType() == QMetaType::QVariantMap) {
            QVariantMap eventMap = value.toMap();
            
            // Extract event fields and create EssEvent
            if (eventMap.contains("e_type") && eventMap.contains("e_subtype")) {
                EssEvent event;
                event.type = static_cast<uint8_t>(eventMap["e_type"].toUInt());
                event.subtype = static_cast<uint8_t>(eventMap["e_subtype"].toUInt());
                event.timestamp = timestamp;
                event.ptype = static_cast<uint8_t>(eventMap.value("e_dtype", 0).toUInt());
                
                // Handle e_params
                if (eventMap.contains("e_params")) {
                    QVariant params = eventMap["e_params"];
                    event.params = QJsonValue::fromVariant(params);
                }
                
                // Process the event
                m_eventProcessor->processEvent(event);
            }
        } else {

        }
        
        return;
    }
    
    // Route based on datapoint name patterns
    
    // Eye tracking data (ain/eye_*)
    if (name.startsWith("ain/eye_")) {
        routeEyeData(name, value, timestamp);
    }
    // ESS system data (ess/*)
    else if (name.startsWith("ess/")) {
        routeEssData(name, value, timestamp);
    }
    // DG data tables (*dg)
    else if (name.endsWith("dg")) {
        routeDgData(name, value, timestamp);
    }
    // Everything else
    else {
        emit genericDatapointReceived(name, value, timestamp);
    }
}

void EssDataProcessor::routeEyeData(const QString &name, const QVariant &value, qint64 timestamp)
{
    if (name == "ain/eye_x" || name == "ain/eye_y") {
        // Need both X and Y for position - would need to cache and combine
        // For now, just emit generic
        emit genericDatapointReceived(name, value, timestamp);
    }
    else if (name == "ain/eye_pos") {
        // Combined position data
        QPointF pos = parseEyePosition(value);
        emit eyePositionUpdated(pos, timestamp);
    }
    else if (name == "ain/eye_vel") {
        // Velocity data
        QPointF vel = parseEyePosition(value); // Same format as position
        emit eyeVelocityUpdated(vel, timestamp);
    }
}

void EssDataProcessor::routeEssData(const QString &name, const QVariant &value, qint64 timestamp)
{
    if (name == "ess/events") {
        QString eventType, eventData;
        parseExperimentEvent(value, eventType, eventData);
        emit experimentEventReceived(eventType, eventData, timestamp);
    }
    else if (name == "ess/status") {
        emit systemStatusUpdated(value.toString());
    }
    else if (name == "ess/state") {
        emit experimentStateChanged(value.toString());
    }
    else if (name == "ess/system" || name == "ess/protocol" || name == "ess/variant") {
        // Collect all three to emit systemConnected
        // For now, just emit generic
        emit genericDatapointReceived(name, value, timestamp);
    }
    else if (name.startsWith("ess/param/")) {
        QString paramName = name.mid(10); // Remove "ess/param/"
        emit parameterChanged(paramName, value);
    }
    else {
        emit genericDatapointReceived(name, value, timestamp);
    }
}

void EssDataProcessor::routeDgData(const QString &name, const QVariant &value, qint64 timestamp)
{
    QByteArray dgData = value.toByteArray();
    
    if (name == "stimdg") {
        emit stimulusDataReceived(dgData, timestamp);
    }
    else if (name == "trialdg") {
        emit trialDataReceived(dgData, timestamp);
    }
    else {
        emit genericDatapointReceived(name, value, timestamp);
    }
}

QPointF EssDataProcessor::parseEyePosition(const QVariant &data)
{
    // Parse eye position data
    // Format might be "x y" string or JSON object
    QString str = data.toString();
    QStringList parts = str.split(' ');
    if (parts.size() >= 2) {
        bool ok1, ok2;
        float x = parts[0].toFloat(&ok1);
        float y = parts[1].toFloat(&ok2);
        if (ok1 && ok2) {
            return QPointF(x, y);
        }
    }
    
    // Try JSON format
    QJsonDocument doc = QJsonDocument::fromJson(data.toByteArray());
    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        return QPointF(obj["x"].toDouble(), obj["y"].toDouble());
    }
    
    return QPointF();
}

void EssDataProcessor::parseExperimentEvent(const QVariant &data, QString &type, QString &eventData)
{
    // Parse experiment event
    // Format might vary - could be string or JSON
    QString str = data.toString();
    int spaceIndex = str.indexOf(' ');
    if (spaceIndex > 0) {
        type = str.left(spaceIndex);
        eventData = str.mid(spaceIndex + 1);
    } else {
        type = str;
        eventData = "";
    }
}

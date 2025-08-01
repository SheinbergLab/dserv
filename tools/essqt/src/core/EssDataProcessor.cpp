#include "EssDataProcessor.h"
#include "console/EssOutputConsole.h"
#include "core/EssEventProcessor.h"
#include "core/EssEvent.h"
#include "EssApplication.h"
#include "EssCommandInterface.h"
#include "DservEventParser.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

extern "C" {
#include "dlfuncs.h"
}

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

void EssDataProcessor::processDatapoint(const QString &name,
										const QVariant &value, 
										qint64 timestamp, int dtype)
{
    // Check if this is a DynGroup by dtype
    if (dtype == DSERV_DG) {
        routeDgData(name, value, timestamp);
        return;
    }
    
    // Handle eventlog/events which has dtype DSERV_EVT
    if (dtype == DSERV_EVT && name == "eventlog/events") {
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
        }
        return;
    }
    
    // Route based on datapoint name patterns for non-DG data
    
    // Eye tracking data (ain/eye_*)
    if (name.startsWith("ain/eye_")) {
        routeEyeData(name, value, timestamp);
    }
    // ESS system data (ess/*)
    else if (name.startsWith("ess/")) {
        routeEssData(name, value, timestamp);
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
    // For DSERV_DG type, the data comes as a base64 string
    QByteArray dgData;
    
    if (value.userType() == QMetaType::QString) {
        // It's a base64 encoded string
        dgData = value.toString().toUtf8();
    } else if (value.userType() == QMetaType::QByteArray) {
        // Already a byte array
        dgData = value.toByteArray();
    } else {
        EssConsoleManager::instance()->logError(
            QString("Unexpected data type for DG %1: %2").arg(name).arg(value.typeName()), 
            "DataProcessor"
        );
        return;
    }
    
    // Decode and register the DG in Tcl
    if (processDynGroup(name, dgData)) {
        // Successfully decoded and added to Tcl
        EssConsoleManager::instance()->logInfo(
            QString("Decoded and registered DynGroup: %1").arg(name), 
            "DataProcessor"
        );
    }
    
    // Still emit specific signals for known DG names for backward compatibility
    if (name == "stimdg") {
        emit stimulusDataReceived(dgData, timestamp);
    }
    else if (name == "trialdg") {
        emit trialDataReceived(dgData, timestamp);
    }
    else {
        // Generic DG received
        emit genericDatapointReceived(name, value, timestamp);
    }
}
bool EssDataProcessor::processDynGroup(const QString &name, const QByteArray &data)
{
    // Get the command interface to access Tcl interpreter
    auto* app = EssApplication::instance();
    if (!app) return false;
    
    auto* cmdInterface = app->commandInterface();
    if (!cmdInterface) return false;
    
    // Get Tcl interpreter
    Tcl_Interp* interp = cmdInterface->tclInterp();
    if (!interp) return false;
    
    // First, check if this DG already exists and remove it
    DYN_GROUP* oldDg = nullptr;
    if (tclFindDynGroup(interp, const_cast<char*>(name.toUtf8().constData()), &oldDg) == TCL_OK) {
        // Delete the old DG from Tcl
        // We need to unset it from the Tcl hash table
        QString deleteCmd = QString("catch {dg_delete %1}").arg(name);
        Tcl_Eval(interp, deleteCmd.toUtf8().constData());
        
        EssConsoleManager::instance()->logDebug(
            QString("Removed existing DynGroup: %1").arg(name), 
            "DataProcessor"
        );
    }
    
    // Decode the new DG
    DYN_GROUP* dg = decode_dg(data.constData(), data.length());
    if (!dg) {
        EssConsoleManager::instance()->logError(
            QString("Failed to decode DynGroup: %1").arg(name), 
            "DataProcessor"
        );
        return false;
    }
    
    // Set the name if not already set
    if (!DYN_GROUP_NAME(dg)[0]) {
        // Use the datapoint name as the DG name
        strncpy(DYN_GROUP_NAME(dg), name.toUtf8().constData(), 
                sizeof(DYN_GROUP_NAME(dg)) - 1);
    }
    
    // Register with Tcl interpreter using tclPutDynGroup
    int result = tclPutDynGroup(interp, dg);
    if (result != TCL_OK) {
        const char* errorMsg = Tcl_GetStringResult(interp);
        EssConsoleManager::instance()->logError(
            QString("Failed to register DynGroup %1: %2").arg(name).arg(errorMsg), 
            "DataProcessor"
        );
        // Clean up the DG if registration failed
        dfuFreeDynGroup(dg);
        return false;
    }
    
    // Log success - the DG is now available in Tcl
    const char* registeredName = Tcl_GetStringResult(interp);
    EssConsoleManager::instance()->logInfo(
        QString("DynGroup '%1' updated/registered as '%2'").arg(name).arg(registeredName), 
        "DataProcessor"
    );
    
    return true;
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

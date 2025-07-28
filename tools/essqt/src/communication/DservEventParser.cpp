#include "DservEventParser.h"

DservEventParser::DservEventParser() {
    // Example: register handler for "ess/em_pos"
    registerHandler("ess/em_pos", [](const QString& rawData) -> QVariant {
        int d1, d2;
        float x, y;
        if (sscanf(rawData.toUtf8().constData(), "%d %d %f %f", &d1, &d2, &x, &y) == 4) {
            QVariantMap pos;
            pos["d1"] = d1;
            pos["d2"] = d2;
            pos["x"] = x;
            pos["y"] = y;
            return pos;
        }
        return rawData;
    });
}

void DservEventParser::registerHandler(const QString& name, CustomHandler handler) {
    customHandlers[name] = handler;
}

std::optional<DservEvent> DservEventParser::parse(const QString& jsonText)
{
    QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8());
    if (!doc.isObject()) {
        qDebug() << "DservEventParser: Not a JSON object:" << jsonText;
        return std::nullopt;
    }
    
    QJsonObject obj = doc.object();
    
    DservEvent event;
    event.name = obj["name"].toString();
    event.timestamp = static_cast<qint64>(obj["timestamp"].toDouble());
    event.dtype = obj["dtype"].toInt();
    
    // Special handling for DSERV_EVT type (eventlog/events)
    if (event.dtype == DSERV_EVT && event.name == "eventlog/events") {
        // For EVT type, the event data is directly in the JSON
        QVariantMap eventData;
        
        // Extract event-specific fields
        if (obj.contains("e_type")) {
            eventData["e_type"] = obj["e_type"].toInt();
        }
        if (obj.contains("e_subtype")) {
            eventData["e_subtype"] = obj["e_subtype"].toInt();
        }
        if (obj.contains("e_dtype")) {
            eventData["e_dtype"] = obj["e_dtype"].toInt();
        }
        if (obj.contains("e_params")) {
            // e_params could be string, array, or other JSON value
            eventData["e_params"] = obj["e_params"].toVariant();
        }
        
        event.data = eventData;
    } else {
        // Normal datapoint processing
        QString dataStr = obj["data"].toString();
        
        // Check for custom handler
        if (customHandlers.contains(event.name)) {
            event.data = customHandlers[event.name](dataStr);
        } else {
            event.data = decodeByDtype(event.dtype, dataStr);
        }
    }
    
    return event;
}

QVariant DservEventParser::decodeByDtype(int dtype, const QString& rawData) {
    switch (dtype) {
        case DSERV_STRING:
        case DSERV_SCRIPT:
        case DSERV_TRIGGER_SCRIPT:
            return rawData;

        case DSERV_INT:
        case DSERV_SHORT:
            return rawData.toInt();

        case DSERV_FLOAT:
        case DSERV_DOUBLE:
            return rawData.toDouble();

        case DSERV_JSON: {
            QJsonParseError jsonErr;
            QJsonDocument jsonDoc = QJsonDocument::fromJson(rawData.toUtf8(), &jsonErr);
            if (!jsonDoc.isNull()) {
                return jsonDoc.toVariant();
            } else {
                qWarning() << "Failed to parse embedded JSON:" << jsonErr.errorString();
                return rawData;
            }
        }

        default:
            return rawData;
    }
}

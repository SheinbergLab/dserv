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

std::optional<DservEvent> DservEventParser::parse(const QString& jsonText) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &error);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "Invalid JSON:" << error.errorString();
        return std::nullopt;
    }

    QJsonObject obj = doc.object();
    DservEvent event;
    event.name = obj.value("name").toString();
    event.timestamp = static_cast<qint64>(obj.value("timestamp").toDouble());
    event.dtype = obj.value("dtype").toInt();

    QString rawData = obj.value("data").toString();

    if (customHandlers.contains(event.name)) {
        event.data = customHandlers[event.name](rawData);
    } else {
        event.data = decodeByDtype(event.dtype, rawData);
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

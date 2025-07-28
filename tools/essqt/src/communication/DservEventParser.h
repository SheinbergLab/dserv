#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QString>

enum DservDataType {
    DSERV_BYTE = 0,
    DSERV_STRING = 1,
    DSERV_FLOAT = 2,
    DSERV_DOUBLE = 3,
    DSERV_SHORT = 4,
    DSERV_INT = 5,
    DSERV_DG = 6,
    DSERV_SCRIPT = 7,
    DSERV_TRIGGER_SCRIPT = 8,
    DSERV_EVT = 9,
    DSERV_NONE = 10,
    DSERV_JSON = 11,
    DSERV_UNKNOWN = 12
};

struct DservEvent {
    QString name;
    qint64 timestamp;
    int dtype;
    QVariant data;
};

class DservEventParser {
public:
    using CustomHandler = std::function<QVariant(const QString& rawData)>;

    DservEventParser();

    std::optional<DservEvent> parse(const QString& jsonText);

    void registerHandler(const QString& name, CustomHandler handler);

private:
    QVariant decodeByDtype(int dtype, const QString& rawData);
    QMap<QString, CustomHandler> customHandlers;
};

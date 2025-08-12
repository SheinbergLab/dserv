#include "EssScriptPrototypeStore.h"
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QCryptographicHash>

EssScriptPrototypeStore* EssScriptPrototypeStore::s_instance = nullptr;

const QStringList EssScriptPrototypeStore::INVALID_FILENAME_CHARS = {
    "/", "\\", ":", "*", "?", "\"", "<", ">", "|", "\0"
};

EssScriptPrototypeStore* EssScriptPrototypeStore::instance()
{
    if (!s_instance) {
        s_instance = new EssScriptPrototypeStore();
    }
    return s_instance;
}

EssScriptPrototypeStore::EssScriptPrototypeStore(QObject* parent)
    : QObject(parent)
{
}

QString EssScriptPrototypeStore::getPrototypeDir() const
{
    if (m_prototypeBaseDir.isEmpty()) {
        // Cross-platform directory selection with fallbacks
        QStringList candidateDirs;
        
        // Primary: Application data directory
        candidateDirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        
        // Fallback 1: User documents
        candidateDirs << QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                         .filePath("EssQt/Prototypes");
        
        // Fallback 2: Home directory
        candidateDirs << QDir::home().filePath(".essqt/prototypes");
        
        // Fallback 3: Temp directory
        candidateDirs << QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                         .filePath("essqt_prototypes");
        
        // Find first writable directory
        for (const QString& dir : candidateDirs) {
            QFileInfo info(dir);
            if (info.exists() && info.isWritable()) {
                m_prototypeBaseDir = dir;
                break;
            } else if (QDir().mkpath(dir)) {
                // Try to create it
                QFile testFile(QDir(dir).filePath("test_write.tmp"));
                if (testFile.open(QIODevice::WriteOnly)) {
                    testFile.close();
                    testFile.remove();
                    m_prototypeBaseDir = dir;
                    break;
                }
            }
        }
        
        // If all else fails, use current directory
        if (m_prototypeBaseDir.isEmpty()) {
            m_prototypeBaseDir = QDir::currentPath() + "/essqt_prototypes";
            qWarning() << "Using current directory for prototypes:" << m_prototypeBaseDir;
        }
        
        ensureDirectoryExists(m_prototypeBaseDir);
        qDebug() << "Prototype directory:" << m_prototypeBaseDir;
    }
    
    return m_prototypeBaseDir;
}

QString EssScriptPrototypeStore::sanitizeFileName(const QString& name) const
{
    QString sanitized = name;
    
    // FIX: Replace individual invalid characters, not strings
    static const QString invalidChars = "/\\:*?\"<>|\0";
    
    for (int i = 0; i < sanitized.length(); ++i) {
        if (invalidChars.contains(sanitized[i])) {
            sanitized[i] = '_';
        }
    }
    
    // Limit length for all platforms
    if (sanitized.length() > MAX_FILENAME_LENGTH) {
        sanitized = sanitized.left(MAX_FILENAME_LENGTH - 10) + "_" + 
                   QString::number(qHash(name), 16).right(8);
    }
    
    // Ensure it doesn't start/end with spaces or dots (Windows issues)
    sanitized = sanitized.trimmed();
    while (sanitized.startsWith('.') || sanitized.endsWith('.')) {
        if (sanitized.startsWith('.')) {
            sanitized = sanitized.mid(1);
        }
        if (sanitized.endsWith('.')) {
            sanitized.chop(1);
        }
    }
    
    return sanitized.isEmpty() ? "prototype" : sanitized;
}

QString EssScriptPrototypeStore::getWidgetPrototypeDir(const QString& widgetType) const
{
    return QDir(getPrototypeDir()).filePath(widgetType);
}

QString EssScriptPrototypeStore::getPrototypeFilePath(const QString& widgetType, const QString& name) const
{
    QString widgetDir = getWidgetPrototypeDir(widgetType);
    QString safeFileName = sanitizeFileName(name) + ".json";
    return QDir(widgetDir).filePath(safeFileName);
}

void EssScriptPrototypeStore::ensureDirectoryExists(const QString& path) const
{
    QDir dir;
    if (!dir.mkpath(path)) {
        qWarning() << "Failed to create directory:" << path;
    }
}

QJsonObject EssScriptPrototypeStore::prototypeToJson(const ScriptPrototype& prototype) const
{
    QJsonObject obj;
    obj["name"] = prototype.name;
    obj["description"] = prototype.description;
    obj["content"] = prototype.content;
    obj["author"] = prototype.author;
    obj["created"] = prototype.created.toString(Qt::ISODate);
    obj["modified"] = prototype.modified.toString(Qt::ISODate);
    obj["version"] = prototype.version;
    obj["isProduction"] = prototype.isProduction;
    
    QJsonArray tagsArray;
    for (const QString& tag : prototype.tags) {
        tagsArray.append(tag);
    }
    obj["tags"] = tagsArray;
    
    return obj;
}

ScriptPrototype EssScriptPrototypeStore::prototypeFromJson(const QJsonObject& json) const
{
    ScriptPrototype prototype;
    prototype.name = json["name"].toString();
    prototype.description = json["description"].toString();
    prototype.content = json["content"].toString();
    prototype.author = json["author"].toString();
    prototype.created = QDateTime::fromString(json["created"].toString(), Qt::ISODate);
    prototype.modified = QDateTime::fromString(json["modified"].toString(), Qt::ISODate);
    prototype.version = json["version"].toString();
    prototype.isProduction = json["isProduction"].toBool();
    
    QJsonArray tagsArray = json["tags"].toArray();
    for (const QJsonValue& value : tagsArray) {
        prototype.tags << value.toString();
    }
    
    return prototype;
}

bool EssScriptPrototypeStore::savePrototype(const QString& widgetType, const ScriptPrototype& prototype)
{
    QString filePath = getPrototypeFilePath(widgetType, prototype.name);
    ensureDirectoryExists(QFileInfo(filePath).absolutePath());
    
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(prototypeToJson(prototype));
        file.write(doc.toJson());
        
        emit prototypesSaved(widgetType);
        return true;
    }
    
    qWarning() << "Failed to save prototype to:" << filePath;
    return false;
}

ScriptPrototype EssScriptPrototypeStore::loadPrototype(const QString& widgetType, const QString& name)
{
    QString filePath = getPrototypeFilePath(widgetType, name);
    
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isNull() && doc.isObject()) {
            ScriptPrototype prototype = prototypeFromJson(doc.object());
            emit prototypeLoaded(widgetType, name);
            return prototype;
        }
    }
    
    // Return empty prototype if load failed
    return ScriptPrototype();
}

QStringList EssScriptPrototypeStore::listPrototypes(const QString& widgetType)
{
    QStringList names;
    QString widgetDir = getWidgetPrototypeDir(widgetType);
    
    QDir dir(widgetDir);
    if (dir.exists()) {
        QStringList files = dir.entryList(QStringList("*.json"), QDir::Files);
        for (const QString& file : files) {
            names << QFileInfo(file).baseName();
        }
    }
    
    return names;
}

bool EssScriptPrototypeStore::deletePrototype(const QString& widgetType, const QString& name)
{
    QString filePath = getPrototypeFilePath(widgetType, name);
    return QFile::remove(filePath);
}

bool EssScriptPrototypeStore::markAsProduction(const QString& widgetType, const QString& name)
{
    ScriptPrototype prototype = loadPrototype(widgetType, name);
    if (prototype.name.isEmpty()) {
        return false;
    }
    
    prototype.isProduction = true;
    prototype.modified = QDateTime::currentDateTime();
    if (!prototype.tags.contains("production")) {
        prototype.tags << "production";
    }
    
    return savePrototype(widgetType, prototype);
}

QStringList EssScriptPrototypeStore::getProductionScripts(const QString& widgetType)
{
    QStringList productionScripts;
    QStringList allPrototypes = listPrototypes(widgetType);
    
    for (const QString& name : allPrototypes) {
        ScriptPrototype prototype = loadPrototype(widgetType, name);
        if (prototype.isProduction) {
            productionScripts << name;
        }
    }
    
    return productionScripts;
}

QString EssScriptPrototypeStore::getEmbeddableScript(const QString& widgetType, const QString& name)
{
    ScriptPrototype prototype = loadPrototype(widgetType, name);
    if (prototype.isProduction && !prototype.content.isEmpty()) {
        return prototype.content;
    }
    
    return QString();
}
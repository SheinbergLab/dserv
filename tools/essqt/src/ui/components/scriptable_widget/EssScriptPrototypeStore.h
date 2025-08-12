#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QDir>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QFileInfo>

struct ScriptPrototype {
    QString name;
    QString description;
    QString content;
    QString author;
    QDateTime created;
    QDateTime modified;
    QString version;
    QStringList tags;
    bool isProduction = false;
};

class EssScriptPrototypeStore : public QObject
{
    Q_OBJECT

public:
    static EssScriptPrototypeStore* instance();
    
    // Prototype management
    bool savePrototype(const QString& widgetType, const ScriptPrototype& prototype);
    ScriptPrototype loadPrototype(const QString& widgetType, const QString& name);
    QStringList listPrototypes(const QString& widgetType);
    bool deletePrototype(const QString& widgetType, const QString& name);
    
    // Production readiness
    bool markAsProduction(const QString& widgetType, const QString& name);
    QStringList getProductionScripts(const QString& widgetType);
    QString getEmbeddableScript(const QString& widgetType, const QString& name);

signals:
    void prototypesSaved(const QString& widgetType);
    void prototypeLoaded(const QString& widgetType, const QString& name);

private:
    explicit EssScriptPrototypeStore(QObject* parent = nullptr);
    
    QString getPrototypeDir() const;
    QString getWidgetPrototypeDir(const QString& widgetType) const;
    QString getPrototypeFilePath(const QString& widgetType, const QString& name) const;
    QString sanitizeFileName(const QString& name) const;
    
    QJsonObject prototypeToJson(const ScriptPrototype& prototype) const;
    ScriptPrototype prototypeFromJson(const QJsonObject& json) const;
    
    void ensureDirectoryExists(const QString& path) const;

private:
    static EssScriptPrototypeStore* s_instance;
    mutable QString m_prototypeBaseDir;
    
    // Cross-platform constants
    static constexpr int MAX_FILENAME_LENGTH = 100;
    static const QStringList INVALID_FILENAME_CHARS;
};

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QByteArray>

class EssConfig : public QObject
{
    Q_OBJECT

public:
    explicit EssConfig(QObject *parent = nullptr);
    ~EssConfig();
    
    // Window settings
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    
    QByteArray windowState() const;
    void setWindowState(const QByteArray &state);
    
    // Connection settings
    QString lastHost() const;
    void setLastHost(const QString &host);
    
    int dservPort() const;
    void setDservPort(int port);
    
    int essPort() const;
    void setEssPort(int port);
    
    // General settings
    bool isDarkMode() const;
    void setDarkMode(bool enabled);
    
    void sync();

signals:
    void settingChanged(const QString &key, const QVariant &value);

private:
    QSettings m_settings;
};

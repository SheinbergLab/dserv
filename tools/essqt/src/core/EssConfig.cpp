#include "EssConfig.h"

EssConfig::EssConfig(QObject *parent)
    : QObject(parent)
    , m_settings("ESSLab", "EssQt")
{
}

EssConfig::~EssConfig()
{
    sync();
}

QByteArray EssConfig::windowGeometry() const
{
    return m_settings.value("window/geometry").toByteArray();
}

void EssConfig::setWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue("window/geometry", geometry);
    emit settingChanged("window/geometry", geometry);
}

QByteArray EssConfig::windowState() const
{
    return m_settings.value("window/state").toByteArray();
}

void EssConfig::setWindowState(const QByteArray &state)
{
    m_settings.setValue("window/state", state);
    emit settingChanged("window/state", state);
}

QString EssConfig::lastHost() const
{
    return m_settings.value("connection/lastHost", "localhost").toString();
}

void EssConfig::setLastHost(const QString &host)
{
    m_settings.setValue("connection/lastHost", host);
    emit settingChanged("connection/lastHost", host);
}

int EssConfig::dservPort() const
{
    return m_settings.value("connection/dservPort", 4620).toInt();
}

void EssConfig::setDservPort(int port)
{
    m_settings.setValue("connection/dservPort", port);
    emit settingChanged("connection/dservPort", port);
}

int EssConfig::essPort() const
{
    return m_settings.value("connection/essPort", 2560).toInt();
}

void EssConfig::setEssPort(int port)
{
    m_settings.setValue("connection/essPort", port);
    emit settingChanged("connection/essPort", port);
}

bool EssConfig::isDarkMode() const
{
    return m_settings.value("appearance/darkMode", true).toBool();
}

void EssConfig::setDarkMode(bool enabled)
{
    m_settings.setValue("appearance/darkMode", enabled);
    emit settingChanged("appearance/darkMode", enabled);
}

void EssConfig::sync()
{
    m_settings.sync();
}

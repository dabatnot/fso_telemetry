#include "application_settings.h"

namespace simpit::radar {

ApplicationSettings::ApplicationSettings()
    : m_settings(QSettings::NativeFormat, QSettings::UserScope,
                 QStringLiteral("FS2Open"), QStringLiteral("FsoSimpitRadar"))
{
}

ApplicationSettings::ApplicationSettings(const QString& settingsPath)
    : m_settings(settingsPath, QSettings::IniFormat)
{
}

ConnectionSettings ApplicationSettings::connection() const
{
    ConnectionSettings result;
    result.configured = m_settings.value(QStringLiteral("connection/configured"), false).toBool();
    result.host = m_settings.value(QStringLiteral("connection/host"), result.host).toString().trimmed();
    if (result.host.isEmpty()) {
        // Keep localhost as the form default, but never turn a persisted empty
        // destination into an apparently valid configured connection.
        result.host = QStringLiteral("127.0.0.1");
        result.configured = false;
    }
    const uint configuredPort = m_settings.value(QStringLiteral("connection/port"), result.port).toUInt();
    if (configuredPort >= 1 && configuredPort <= 65535)
        result.port = static_cast<quint16>(configuredPort);
    else
        result.configured = false;
    return result;
}

void ApplicationSettings::setConnection(const ConnectionSettings& connection)
{
    m_settings.setValue(QStringLiteral("connection/configured"), connection.configured);
    m_settings.setValue(QStringLiteral("connection/host"), connection.host);
    m_settings.setValue(QStringLiteral("connection/port"), connection.port);
}

RadarDisplaySettings ApplicationSettings::displaySettings() const
{
    RadarDisplaySettings result;
    result.targetCallout = m_settings.value(QStringLiteral("display/targetCallout"), result.targetCallout).toBool();
    result.targetStrength = m_settings.value(QStringLiteral("display/targetStrength"), result.targetStrength).toBool();
    result.lead = m_settings.value(QStringLiteral("display/lead"), result.lead).toBool();
    result.lock = m_settings.value(QStringLiteral("display/lock"), result.lock).toBool();
    result.subsystems = m_settings.value(QStringLiteral("display/subsystems"), result.subsystems).toBool();
    result.edgeThreats = m_settings.value(QStringLiteral("display/edgeThreats"), result.edgeThreats).toBool();
    result.sensorEffects = m_settings.value(QStringLiteral("display/sensorEffects"), result.sensorEffects).toBool();
    result.motionVectors = m_settings.value(QStringLiteral("display/motionVectors"), result.motionVectors).toBool();
    result.trails = m_settings.value(QStringLiteral("display/trails"), result.trails).toBool();
    return result;
}

void ApplicationSettings::setDisplaySettings(const RadarDisplaySettings& display)
{
    m_settings.setValue(QStringLiteral("display/targetCallout"), display.targetCallout);
    m_settings.setValue(QStringLiteral("display/targetStrength"), display.targetStrength);
    m_settings.setValue(QStringLiteral("display/lead"), display.lead);
    m_settings.setValue(QStringLiteral("display/lock"), display.lock);
    m_settings.setValue(QStringLiteral("display/subsystems"), display.subsystems);
    m_settings.setValue(QStringLiteral("display/edgeThreats"), display.edgeThreats);
    m_settings.setValue(QStringLiteral("display/sensorEffects"), display.sensorEffects);
    m_settings.setValue(QStringLiteral("display/motionVectors"), display.motionVectors);
    m_settings.setValue(QStringLiteral("display/trails"), display.trails);
}

QByteArray ApplicationSettings::windowGeometry() const
{
    return m_settings.value(QStringLiteral("window/geometry")).toByteArray();
}

void ApplicationSettings::setWindowGeometry(const QByteArray& geometry)
{
    m_settings.setValue(QStringLiteral("window/geometry"), geometry);
}

void ApplicationSettings::sync()
{
    m_settings.sync();
}

} // namespace simpit::radar

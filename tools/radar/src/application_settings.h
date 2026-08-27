#pragma once

#include "radar_display_settings.h"

#include <QByteArray>
#include <QSettings>
#include <QString>

namespace simpit::radar {

struct ConnectionSettings final {
    bool configured = false;
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 42042;
};

class ApplicationSettings final {
public:
    ApplicationSettings();
    explicit ApplicationSettings(const QString& settingsPath);

    ConnectionSettings connection() const;
    void setConnection(const ConnectionSettings& connection);
    RadarDisplaySettings displaySettings() const;
    void setDisplaySettings(const RadarDisplaySettings& display);
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);
    void sync();

private:
    QSettings m_settings;
};

} // namespace simpit::radar

#pragma once

#include <QCoreApplication>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>

namespace simpit::radar::test {

class ScopedApplicationIdentity final {
public:
    explicit ScopedApplicationIdentity(const QString& applicationName = QStringLiteral("AvDsTests"))
        : m_organizationName(QCoreApplication::organizationName()),
          m_organizationDomain(QCoreApplication::organizationDomain()),
          m_applicationName(QCoreApplication::applicationName()),
          m_applicationVersion(QCoreApplication::applicationVersion())
    {
        QCoreApplication::setOrganizationName(QStringLiteral("FS2Open"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("fs2open.github.com"));
        QCoreApplication::setApplicationName(applicationName);
        QCoreApplication::setApplicationVersion(QStringLiteral("test"));
    }

    ~ScopedApplicationIdentity()
    {
        QCoreApplication::setOrganizationName(m_organizationName);
        QCoreApplication::setOrganizationDomain(m_organizationDomain);
        QCoreApplication::setApplicationName(m_applicationName);
        QCoreApplication::setApplicationVersion(m_applicationVersion);
    }

    ScopedApplicationIdentity(const ScopedApplicationIdentity&) = delete;
    ScopedApplicationIdentity& operator=(const ScopedApplicationIdentity&) = delete;

private:
    QString m_organizationName;
    QString m_organizationDomain;
    QString m_applicationName;
    QString m_applicationVersion;
};

class TemporarySettings final {
public:
    TemporarySettings()
    {
        Q_ASSERT(m_directory.isValid());
    }

    QSettings open() const
    {
        return QSettings(settingsPath(), QSettings::IniFormat);
    }

    QString settingsPath() const
    {
        return m_directory.filePath(QStringLiteral("settings.ini"));
    }

private:
    QTemporaryDir m_directory;
};

inline QImage renderOffscreen(QWidget& widget, const QSize& size)
{
    widget.resize(size);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

} // namespace simpit::radar::test

#include "main_window.h"

#include "radar_widget.h"
#include "settings_dialog.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QTimer>

namespace simpit::radar {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_radar(new RadarWidget(this))
{
    setWindowTitle(tr("SimPit Radar"));
    setCentralWidget(m_radar);
    setMinimumSize(320, 320);
    restoreWindowGeometry();
    connect(&m_client, &RadarClient::imageReady, m_radar, &RadarWidget::setImage);
    connect(&m_client, &RadarClient::statusChanged, m_radar, &RadarWidget::setStatus);

    QSettings settings;
    const bool configured = settings.value(QStringLiteral("connection/configured"), false).toBool();
    m_host = settings.value(QStringLiteral("connection/host"), QStringLiteral("127.0.0.1")).toString();
    m_port = static_cast<quint16>(settings.value(QStringLiteral("connection/port"), 42042).toUInt());
    m_display.targetCallout = settings.value(QStringLiteral("display/targetCallout"), true).toBool();
    m_display.targetStrength = settings.value(QStringLiteral("display/targetStrength"), true).toBool();
    m_display.lead = settings.value(QStringLiteral("display/lead"), true).toBool();
    m_display.lock = settings.value(QStringLiteral("display/lock"), true).toBool();
    m_display.subsystems = settings.value(QStringLiteral("display/subsystems"), true).toBool();
    m_display.edgeThreats = settings.value(QStringLiteral("display/edgeThreats"), true).toBool();
    m_display.sensorEffects = settings.value(QStringLiteral("display/sensorEffects"), true).toBool();
    m_display.motionVectors = settings.value(QStringLiteral("display/motionVectors"), false).toBool();
    m_display.trails = settings.value(QStringLiteral("display/trails"), false).toBool();
    m_radar->setDisplaySettings(m_display);
    if (configured) {
        QTimer::singleShot(0, this, &MainWindow::connectConfiguredDestination);
    } else {
        QTimer::singleShot(0, this, [this] { openSettings(true); });
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::restoreWindowGeometry()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) resize(720, 720);
}

void MainWindow::connectConfiguredDestination()
{
    m_client.start(m_host, m_port);
}

void MainWindow::openSettings(bool firstRun)
{
    SettingsDialog dialog(m_host.isEmpty() ? QStringLiteral("127.0.0.1") : m_host,
                          m_port, m_display, this);
    if (dialog.exec() == QDialog::Accepted) {
        const QString previousHost = m_host;
        const quint16 previousPort = m_port;
        m_host = dialog.host();
        m_port = dialog.port();
        m_display = dialog.displaySettings();
        m_radar->setDisplaySettings(m_display);
        QSettings settings;
        settings.setValue(QStringLiteral("connection/configured"), true);
        settings.setValue(QStringLiteral("connection/host"), m_host);
        settings.setValue(QStringLiteral("connection/port"), m_port);
        settings.setValue(QStringLiteral("display/targetCallout"), m_display.targetCallout);
        settings.setValue(QStringLiteral("display/targetStrength"), m_display.targetStrength);
        settings.setValue(QStringLiteral("display/lead"), m_display.lead);
        settings.setValue(QStringLiteral("display/lock"), m_display.lock);
        settings.setValue(QStringLiteral("display/subsystems"), m_display.subsystems);
        settings.setValue(QStringLiteral("display/edgeThreats"), m_display.edgeThreats);
        settings.setValue(QStringLiteral("display/sensorEffects"), m_display.sensorEffects);
        settings.setValue(QStringLiteral("display/motionVectors"), m_display.motionVectors);
        settings.setValue(QStringLiteral("display/trails"), m_display.trails);
        if (firstRun || previousHost != m_host || previousPort != m_port)
            connectConfiguredDestination();
    } else if (firstRun) {
        m_radar->setStatus(ClientStatus::Disconnected, tr("Press Escape or Ctrl+, to configure"));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape ||
        (event->key() == Qt::Key_Comma && event->modifiers().testFlag(Qt::ControlModifier))) {
        openSettings();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

} // namespace simpit::radar

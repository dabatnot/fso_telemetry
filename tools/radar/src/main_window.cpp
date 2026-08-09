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
    setWindowTitle(tr("Radar SimPit"));
    setCentralWidget(m_radar);
    setMinimumSize(320, 320);
    restoreWindowGeometry();
    connect(&m_client, &RadarClient::imageReady, m_radar, &RadarWidget::setImage);
    connect(&m_client, &RadarClient::statusChanged, m_radar, &RadarWidget::setStatus);

    QSettings settings;
    const bool configured = settings.value(QStringLiteral("connection/configured"), false).toBool();
    m_host = settings.value(QStringLiteral("connection/host"), QStringLiteral("127.0.0.1")).toString();
    m_port = static_cast<quint16>(settings.value(QStringLiteral("connection/port"), 42042).toUInt());
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
                          m_port, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_host = dialog.host();
        m_port = dialog.port();
        QSettings settings;
        settings.setValue(QStringLiteral("connection/configured"), true);
        settings.setValue(QStringLiteral("connection/host"), m_host);
        settings.setValue(QStringLiteral("connection/port"), m_port);
        connectConfiguredDestination();
    } else if (firstRun) {
        m_radar->setStatus(ClientStatus::Disconnected, tr("Échap ou Ctrl+, pour configurer"));
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


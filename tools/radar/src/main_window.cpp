#include "main_window.h"

#include "av_ds_message.h"
#include "radar_widget.h"
#include "settings_dialog.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QTimer>

namespace simpit::radar {

MainWindow::MainWindow(QWidget* parent)
    : MainWindow(std::make_unique<ApplicationSettings>(), parent)
{
}

MainWindow::MainWindow(std::unique_ptr<ApplicationSettings> settings, QWidget* parent)
    : QMainWindow(parent), m_settings(std::move(settings)), m_radar(new RadarWidget),
      m_displayUnit(new DisplayUnit(DisplayUnitId::MfdLeft, m_radar, this))
{
    m_displayUnits = {m_displayUnit};
    setWindowTitle(tr("AV DS — AV Display System"));
    setCentralWidget(m_displayUnit);
    setMinimumSize(320, 320);
    restoreWindowGeometry();
    connect(&m_client, &RadarClient::imageReady, m_radar, &RadarWidget::setImage);
    connect(&m_client, &RadarClient::statusChanged, this,
            [this](ClientStatus status, const QString& detail) {
                setClientStatus(status, detail);
            });

    const ConnectionSettings connection = m_settings->connection();
    m_host = connection.host;
    m_port = connection.port;
    m_display = m_settings->displaySettings();
    m_radar->setDisplaySettings(m_display);
    if (connection.configured) {
        QTimer::singleShot(0, this, &MainWindow::connectConfiguredDestination);
    } else {
        QTimer::singleShot(0, this, [this] { openSettings(true); });
    }
}

MainWindow::~MainWindow() = default;

DisplayUnit* MainWindow::displayUnit() const noexcept
{
    return m_displayUnit;
}

void MainWindow::restoreWindowGeometry()
{
    const QByteArray geometry = m_settings->windowGeometry();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) resize(720, 720);
}

void MainWindow::connectConfiguredDestination()
{
    m_client.start(m_host, m_port);
}

void MainWindow::setClientStatus(ClientStatus status, const QString& detail)
{
    const AvDsMessage message = messageForClientStatus(status, detail);
    for (DisplayUnit* unit : m_displayUnits) unit->setMessage(message);
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
        m_settings->setConnection({true, m_host, m_port});
        m_settings->setDisplaySettings(m_display);
        m_settings->sync();
        if (firstRun || previousHost != m_host || previousPort != m_port)
            connectConfiguredDestination();
    } else if (firstRun) {
        setClientStatus(
            ClientStatus::Disconnected, tr("Press Escape or Ctrl+, to configure"));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_settings->setWindowGeometry(saveGeometry());
    m_settings->sync();
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

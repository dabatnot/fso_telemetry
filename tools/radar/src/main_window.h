#pragma once

#include "radar_client.h"
#include "radar_display_settings.h"

#include <QMainWindow>

namespace simpit::radar {

class RadarWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void openSettings(bool firstRun = false);

private:
    void connectConfiguredDestination();
    void restoreWindowGeometry();

    RadarWidget* m_radar = nullptr;
    RadarClient m_client;
    QString m_host;
    quint16 m_port = 42042;
    RadarDisplaySettings m_display;
};

} // namespace simpit::radar

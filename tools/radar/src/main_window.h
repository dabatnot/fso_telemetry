#pragma once

#include "application_settings.h"
#include "display_unit.h"
#include "radar_client.h"
#include "radar_display_settings.h"

#include <QMainWindow>

#include <memory>

namespace simpit::radar {

class RadarWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    MainWindow(std::unique_ptr<ApplicationSettings> settings, QWidget* parent = nullptr);
    ~MainWindow() override;

    DisplayUnit* displayUnit() const noexcept;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void openSettings(bool firstRun = false);

private:
    void connectConfiguredDestination();
    void restoreWindowGeometry();

    std::unique_ptr<ApplicationSettings> m_settings;
    RadarWidget* m_radar = nullptr;
    DisplayUnit* m_displayUnit = nullptr;
    RadarClient m_client;
    QString m_host;
    quint16 m_port = 42042;
    RadarDisplaySettings m_display;
};

} // namespace simpit::radar

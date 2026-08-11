#pragma once

#include "radar_display_settings.h"

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QCheckBox;

namespace simpit::radar {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const QString& host, quint16 port,
                            const RadarDisplaySettings& display,
                            QWidget* parent = nullptr);

    QString host() const;
    quint16 port() const;
    RadarDisplaySettings displaySettings() const;

private:
    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
    QCheckBox* m_targetCallout = nullptr;
    QCheckBox* m_targetStrength = nullptr;
    QCheckBox* m_lead = nullptr;
    QCheckBox* m_lock = nullptr;
    QCheckBox* m_subsystems = nullptr;
    QCheckBox* m_edgeThreats = nullptr;
    QCheckBox* m_sensorEffects = nullptr;
    QCheckBox* m_motionVectors = nullptr;
    QCheckBox* m_trails = nullptr;
};

} // namespace simpit::radar

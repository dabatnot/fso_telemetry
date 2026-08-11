#include "settings_dialog.h"

#include <QDialogButtonBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace simpit::radar {

SettingsDialog::SettingsDialog(const QString& host, quint16 port,
                               const RadarDisplaySettings& display, QWidget* parent)
    : QDialog(parent), m_host(new QLineEdit(host, this)), m_port(new QSpinBox(this))
{
    setWindowTitle(tr("Radar connection"));
    setModal(true);
    m_host->setPlaceholderText(QStringLiteral("127.0.0.1"));
    m_host->setClearButtonEnabled(true);
    m_port->setRange(1, 65535);
    m_port->setValue(port == 0 ? 42042 : port);
    auto* form = new QFormLayout;
    form->addRow(tr("Host"), m_host);
    form->addRow(tr("Port"), m_port);
    auto* enhanced = new QGroupBox(tr("Enhanced display"), this);
    auto* enhancedLayout = new QVBoxLayout(enhanced);
    const auto addOption = [enhanced, enhancedLayout](QCheckBox*& box,
                                                       const QString& text, bool checked) {
        box = new QCheckBox(text, enhanced);
        box->setChecked(checked);
        enhancedLayout->addWidget(box);
    };
    addOption(m_targetCallout, tr("Target Callout"), display.targetCallout);
    addOption(m_targetStrength, tr("Hull and shields"), display.targetStrength);
    addOption(m_lead, tr("Lead indicator"), display.lead);
    addOption(m_lock, tr("Lock progress"), display.lock);
    addOption(m_subsystems, tr("Subsystems"), display.subsystems);
    addOption(m_edgeThreats, tr("Edge threats"), display.edgeThreats);
    addOption(m_sensorEffects, tr("Sensor and EMP effects"), display.sensorEffects);
    addOption(m_motionVectors, tr("Motion vectors"), display.motionVectors);
    addOption(m_trails, tr("Trails"), display.trails);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_host->text().trimmed().isEmpty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Direct connection to FS2Open (FSTL 1.1 / CockpitSensors)"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(enhanced);
    layout->addWidget(buttons);
    setMinimumWidth(380);
}

QString SettingsDialog::host() const { return m_host->text().trimmed(); }
quint16 SettingsDialog::port() const { return static_cast<quint16>(m_port->value()); }

RadarDisplaySettings SettingsDialog::displaySettings() const
{
    return {m_targetCallout->isChecked(), m_targetStrength->isChecked(),
            m_lead->isChecked(), m_lock->isChecked(), m_subsystems->isChecked(),
            m_edgeThreats->isChecked(), m_sensorEffects->isChecked(),
            m_motionVectors->isChecked(), m_trails->isChecked()};
}

} // namespace simpit::radar

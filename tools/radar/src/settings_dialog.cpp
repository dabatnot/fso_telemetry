#include "settings_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace simpit::radar {

SettingsDialog::SettingsDialog(const QString& host, quint16 port, QWidget* parent)
    : QDialog(parent), m_host(new QLineEdit(host, this)), m_port(new QSpinBox(this))
{
    setWindowTitle(tr("Connexion radar"));
    setModal(true);
    m_host->setPlaceholderText(QStringLiteral("127.0.0.1"));
    m_host->setClearButtonEnabled(true);
    m_port->setRange(1, 65535);
    m_port->setValue(port == 0 ? 42042 : port);
    auto* form = new QFormLayout;
    form->addRow(tr("Hôte"), m_host);
    form->addRow(tr("Port"), m_port);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Connexion"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_host->text().trimmed().isEmpty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Connexion directe à FS2Open (FSTL 1.1 / CockpitSensors)"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(buttons);
    setMinimumWidth(380);
}

QString SettingsDialog::host() const { return m_host->text().trimmed(); }
quint16 SettingsDialog::port() const { return static_cast<quint16>(m_port->value()); }

} // namespace simpit::radar


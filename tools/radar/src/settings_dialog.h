#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;

namespace simpit::radar {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const QString& host, quint16 port, QWidget* parent = nullptr);

    QString host() const;
    quint16 port() const;

private:
    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
};

} // namespace simpit::radar


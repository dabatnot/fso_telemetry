#pragma once

#include "radar_client.h"

#include <QColor>
#include <QString>

namespace simpit::radar {

struct AvDsMessage final {
    QString title;
    QString detail;
    QColor color;
    bool visible = false;
    bool dimsContent = false;
};

AvDsMessage messageForClientStatus(ClientStatus status, const QString& detail = {});

} // namespace simpit::radar

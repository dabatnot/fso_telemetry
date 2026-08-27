#include "av_ds_message.h"

namespace simpit::radar {
namespace {

const QColor LinkColor(102, 217, 232);
const QColor StaleColor(255, 184, 61);
const QColor FailureColor(255, 92, 87);

} // namespace

AvDsMessage messageForClientStatus(ClientStatus status, const QString& detail)
{
    switch (status) {
    case ClientStatus::Resolving:
    case ClientStatus::Connecting:
        return {QObject::tr("ESTABLISHING SENSOR LINK"), QObject::tr("STANDBY"), LinkColor, true};
    case ClientStatus::Synchronizing:
        return {QObject::tr("BUILDING TACTICAL PICTURE"), QObject::tr("STANDBY"), LinkColor, true};
    case ClientStatus::Ready:
        return {QObject::tr("SENSOR LINK READY"), QObject::tr("WAITING FOR MISSION"), LinkColor, true};
    case ClientStatus::Paused:
        return {QObject::tr("MISSION PAUSED"), QObject::tr("SENSOR LINK MAINTAINED"), LinkColor, true};
    case ClientStatus::Stale:
        return {QObject::tr("SENSOR FEED LOST"), QObject::tr("HOLDING LAST CONTACT PICTURE"), StaleColor, true, true};
    case ClientStatus::Reconnecting:
        return {QObject::tr("REACQUIRING SENSOR LINK"), QObject::tr("STANDBY"), LinkColor, true};
    case ClientStatus::Error:
        return {QObject::tr("SENSOR LINK FAILURE"), QObject::tr("CHECK TELEMETRY SOURCE"), FailureColor, true};
    case ClientStatus::Disconnected:
        return {QObject::tr("SENSOR LINK DISCONNECTED"),
                detail.isEmpty() ? QObject::tr("PRESS ESCAPE OR CTRL+, TO CONFIGURE") : detail,
                FailureColor, true};
    case ClientStatus::Live:
        return {};
    }
    return {};
}

} // namespace simpit::radar

#pragma once

#include "radar_image.h"
#include "telemetry/protocol/telemetry_protocol_types.h"

#include <QObject>
#include <QString>

class QThread;

namespace simpit::radar {

bool decodeFstl11SnapshotRegion(telemetry::protocol::ByteView records,
                                std::uint16_t recordCount,
                                telemetry::protocol::StateImage& output) noexcept;

enum class ClientStatus : std::uint8_t {
    Disconnected,
    Resolving,
    Connecting,
    Synchronizing,
	Ready,
    Live,
	Paused,
    Stale,
    Reconnecting,
    Error,
};

ClientStatus statusForSilence(qint64 silenceMilliseconds,
                              bool sessionEstablished,
                              bool baselineApplied = true) noexcept;

class RadarClientWorker;

class RadarClient final : public QObject {
    Q_OBJECT
public:
    explicit RadarClient(QObject* parent = nullptr);
    ~RadarClient() override;

    void start(const QString& host, quint16 port);
    void stop();

signals:
    void imageReady(std::shared_ptr<const RadarImage> image);
    void statusChanged(ClientStatus status, const QString& detail);

private:
    QThread* m_thread = nullptr;
    RadarClientWorker* m_worker = nullptr;
};

} // namespace simpit::radar

Q_DECLARE_METATYPE(simpit::radar::ClientStatus)

#include "radar_client.h"

#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliable_receive.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_session_context.h"
#include "telemetry/protocol/telemetry_state_messages.h"
#include "telemetry/protocol/telemetry_transaction.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QNetworkDatagram>
#include <QPointer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace simpit::radar {
namespace protocol = telemetry::protocol;

ClientStatus statusForSilence(qint64 silenceMilliseconds, bool sessionEstablished) noexcept
{
    if (!sessionEstablished) return ClientStatus::Connecting;
    if (silenceMilliseconds >= 10'000) return ClientStatus::Reconnecting;
    if (silenceMilliseconds >= 3'000) return ClientStatus::Stale;
    return ClientStatus::Live;
}

namespace {

class Fstl11StateImageValidator final : public protocol::StateImageValidator {
public:
    protocol::ValidationError validate(const protocol::StateImage&) const noexcept override
    {
        // Radar-specific semantic validation is performed by makeRadarImage()
        // before the candidate is published. This validator selects the
        // negotiated 1.1 business-record catalogue and keeps construction
        // atomic, including cascade-owner checks.
        return protocol::ValidationError::None;
    }

    std::uint8_t protocol_minor() const noexcept override
    {
        return protocol::VersionMinorV1_1;
    }
};

protocol::ByteView bytes(const QByteArray& value)
{
    return {reinterpret_cast<const std::uint8_t*>(value.constData()),
            static_cast<std::size_t>(value.size())};
}

QByteArray toByteArray(const std::vector<std::uint8_t>& value)
{
    return QByteArray(reinterpret_cast<const char*>(value.data()),
                      static_cast<qsizetype>(value.size()));
}

protocol::EndpointKey endpointKey(const QHostAddress& address, quint16 port)
{
    bool ok = false;
    const quint32 ipv4 = address.toIPv4Address(&ok);
    if (ok) {
        return protocol::EndpointKey::from_ipv4(
            {static_cast<std::uint8_t>((ipv4 >> 24U) & 0xffU),
             static_cast<std::uint8_t>((ipv4 >> 16U) & 0xffU),
             static_cast<std::uint8_t>((ipv4 >> 8U) & 0xffU),
             static_cast<std::uint8_t>(ipv4 & 0xffU)}, port);
    }
    const Q_IPV6ADDR ipv6 = address.toIPv6Address();
    std::array<std::uint8_t, 16> raw{};
    std::copy(std::begin(ipv6.c), std::end(ipv6.c), raw.begin());
    return protocol::EndpointKey::from_ipv6(raw, port);
}

QString messageKey(const protocol::TelemetryDatagramHeader& header)
{
    return QStringLiteral("%1:%2:%3:%4")
        .arg(header.session_id)
        .arg(static_cast<unsigned>(header.message_type))
        .arg(header.message_id)
        .arg(header.message_crc32);
}

protocol::TransactionPart transactionPart(const protocol::ManifestPartPayload& payload,
                                          const protocol::TelemetryDatagramHeader& header)
{
    protocol::TransactionPart result;
    result.session_id = header.session_id;
    result.message_type = protocol::MessageType::Manifest;
    result.transaction_id = payload.manifest_id;
    result.message_id = header.message_id;
    result.part_index = payload.part_index;
    result.part_count = payload.part_count;
    result.transaction_size = payload.transaction_size;
    result.transaction_sha256 = payload.transaction_sha256;
    result.producer_sample_time_us = payload.producer_sample_time_us;
    result.kind_or_flags = static_cast<std::uint16_t>(payload.manifest_kind);
    result.record_count = payload.record_count;
    result.records = payload.records;
    return result;
}

protocol::TransactionPart transactionPart(const protocol::FullSnapshotPartPayload& payload,
                                          const protocol::TelemetryDatagramHeader& header)
{
    protocol::TransactionPart result;
    result.session_id = header.session_id;
    result.message_type = protocol::MessageType::FullSnapshot;
    result.transaction_id = payload.snapshot_id;
    result.message_id = header.message_id;
    result.part_index = payload.part_index;
    result.part_count = payload.part_count;
    result.transaction_size = payload.transaction_size;
    result.transaction_sha256 = payload.transaction_sha256;
    result.producer_sample_time_us = payload.producer_sample_time_us;
    result.frame_id = header.frame_id;
    result.mission_time_us = header.mission_time_us;
    result.kind_or_flags = payload.snapshot_flags;
    result.required_manifest_id = payload.required_manifest_id;
    result.record_count = payload.record_count;
    result.records = payload.records;
    return result;
}

} // namespace

bool decodeFstl11SnapshotRegion(protocol::ByteView records,
                                std::uint16_t recordCount,
                                protocol::StateImage& output) noexcept
{
    const Fstl11StateImageValidator validator;
    return protocol::decode_business_snapshot_region_validated(
               records, recordCount, validator, output) == protocol::ValidationError::None;
}

class RadarClientWorker final : public QObject {
    Q_OBJECT
public:
    explicit RadarClientWorker(QObject* parent = nullptr) : QObject(parent)
    {
        // The worker is constructed on the UI thread and moved afterwards.
        // Parenting the value-member timer makes it follow the worker to the
        // network thread; an unparented timer cannot be started there.
        m_tick.setParent(this);
        m_tick.setInterval(100);
        connect(&m_tick, &QTimer::timeout, this, &RadarClientWorker::onTick);
    }

public slots:
    void startSession(const QString& host, quint16 port)
    {
        m_host = host.trimmed();
        m_port = port;
        m_reconnectGeneration = 0;
        openEndpoint(false);
    }

    void stopSession()
    {
        ++m_endpointGeneration;
        m_tick.stop();
        resetProtocol();
        if (m_socket != nullptr) {
            m_socket->close();
            m_socket->deleteLater();
            m_socket = nullptr;
        }
        emit statusChanged(ClientStatus::Disconnected, {});
    }

signals:
    void imageReady(std::shared_ptr<const RadarImage> image);
    void statusChanged(ClientStatus status, const QString& detail);

private:
    std::uint64_t nowUs() const
    {
        return static_cast<std::uint64_t>(m_clock.nsecsElapsed() / 1000);
    }

    void resetProtocol()
    {
        // A reconnect resolves and binds a brand-new UDP endpoint.  Keeping the
        // previous peer address here lets the still-running timer attempt a
        // HELLO before lookupHost() completes; that empty HELLO used to stop
        // the retry timer permanently after a producer restart.
        m_endpointReady = false;
        m_peerAddress.clear();
        m_peerEndpoint = {};
        m_sessionId = 0;
        m_nonce = 0;
        m_sequence = 0;
        m_messageId = 0;
        m_manifestId = 0;
        m_baselineSnapshotId = 0;
        m_lastDeltaSequence = 0;
        m_hasBaseline = false;
        m_sessionBegun = false;
        m_lastProgressUs = 0;
        m_lastHelloUs = 0;
        m_helloFirstUs = 0;
        m_helloT0Us = 0;
        m_helloMessageId = 0;
        m_helloPayload.clear();
        m_staleSignalled = false;
        m_resyncPending = false;
        m_reassembler.clear();
        m_transactions.clear();
        m_ackLevels.clear();
        m_reliableHeaders.clear();
        m_baseline = {};
        m_current = {};
    }

    void openEndpoint(bool reconnecting)
    {
        if (m_host.isEmpty() || m_port == 0) {
            fail(tr("Destination invalide"));
            return;
        }
        if (!m_clock.isValid()) m_clock.start();
        resetProtocol();
        if (m_socket != nullptr) {
            disconnect(m_socket, nullptr, this, nullptr);
            m_socket->close();
            m_socket->deleteLater();
        }
        m_socket = new QUdpSocket(this);
        connect(m_socket, &QUdpSocket::readyRead, this, &RadarClientWorker::readPendingDatagrams);
        connect(m_socket, &QUdpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
            // A connected UDP socket reports an ICMP port-unreachable as
            // ConnectionRefusedError (WSAECONNRESET on Windows). It is not a
            // session failure; the silence timers own recovery.
            if (error == QAbstractSocket::ConnectionRefusedError) return;
            if (error != QAbstractSocket::UnknownSocketError) {
                const QString detail = m_socket != nullptr
                    ? m_socket->errorString() : tr("Erreur réseau UDP");
                retryEndpoint(detail);
            }
        });
        emit statusChanged(reconnecting ? ClientStatus::Reconnecting : ClientStatus::Resolving,
                           reconnecting ? tr("Reconnexion…") : tr("Résolution de %1…").arg(m_host));
        const QPointer<RadarClientWorker> guard(this);
        const std::uint64_t generation = ++m_endpointGeneration;
        QHostInfo::lookupHost(m_host, this, [guard, reconnecting, generation](const QHostInfo& info) {
            if (!guard) return;
            guard->hostResolved(info, reconnecting, generation);
        });
        if (!m_tick.isActive()) m_tick.start();
    }

    void hostResolved(const QHostInfo& info, bool reconnecting, std::uint64_t generation)
    {
        if (generation != m_endpointGeneration) return;
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            fail(tr("Impossible de résoudre %1 : %2").arg(m_host, info.errorString()));
            return;
        }
        QHostAddress selected;
        for (const QHostAddress& candidate : info.addresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                selected = candidate;
                break;
            }
            if (selected.isNull() && candidate.protocol() == QAbstractSocket::IPv6Protocol) selected = candidate;
        }
        if (selected.isNull()) {
            fail(tr("Aucune adresse IPv4 ou IPv6 utilisable"));
            return;
        }
        m_peerAddress = selected;
        m_peerEndpoint = endpointKey(selected, m_port);
        const QHostAddress bindAddress = selected.protocol() == QAbstractSocket::IPv6Protocol
            ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4;
        if (!m_socket->bind(bindAddress, 0, QUdpSocket::DefaultForPlatform)) {
            fail(tr("Ouverture UDP impossible : %1").arg(m_socket->errorString()));
            return;
        }
        m_socket->connectToHost(m_peerAddress, m_port, QIODevice::ReadWrite);
        m_endpointReady = true;
        emit statusChanged(reconnecting ? ClientStatus::Reconnecting : ClientStatus::Connecting,
                           tr("Connexion à %1:%2…").arg(m_peerAddress.toString()).arg(m_port));
        beginHello();
    }

    void retryEndpoint(const QString& detail, int delayMs = 1000)
    {
        // Invalidate an outstanding DNS callback and make the no-session tick
        // inert until the delayed open has prepared a complete HELLO.
        const std::uint64_t generation = ++m_endpointGeneration;
        resetProtocol();
        if (m_socket != nullptr) {
            disconnect(m_socket, nullptr, this, nullptr);
            m_socket->close();
            m_socket->deleteLater();
            m_socket = nullptr;
        }
        emit statusChanged(ClientStatus::Reconnecting, detail);
        if (!m_tick.isActive()) m_tick.start();
        QTimer::singleShot(delayMs, this, [this, generation]() {
            if (generation == m_endpointGeneration) openEndpoint(true);
        });
    }

    template <typename Payload, typename Encoder>
    bool encodePayload(const Payload& payload, std::size_t capacity, Encoder encoder, QByteArray& output)
    {
        std::vector<std::uint8_t> storage(capacity);
        std::size_t written = 0;
        if (encoder(payload, {storage.data(), storage.size()}, written) != protocol::ValidationError::None) {
            return false;
        }
        storage.resize(written);
        output = toByteArray(storage);
        return true;
    }

    bool sendMessage(protocol::MessageType type, const QByteArray& payload,
                     std::uint8_t flags = protocol::MessageFlagNone,
                     std::uint64_t sessionOverride = std::numeric_limits<std::uint64_t>::max(),
                     std::uint32_t messageOverride = 0)
    {
        if (m_socket == nullptr || m_peerAddress.isNull() || payload.size() >
            static_cast<qsizetype>(protocol::MaxFragmentPayload)) return false;
        protocol::TelemetryDatagramHeader header;
        header.version_minor = protocol::VersionMinorV1_1;
        header.message_type = type;
        header.flags = flags;
        header.session_id = sessionOverride == std::numeric_limits<std::uint64_t>::max()
            ? m_sessionId : sessionOverride;
        header.packet_sequence = ++m_sequence;
        header.sent_time_us = nowUs();
        header.message_id = messageOverride != 0 ? messageOverride : ++m_messageId;
        header.message_size = static_cast<std::uint32_t>(payload.size());
        header.message_crc32 = protocol::crc32_iso_hdlc(bytes(payload));
        std::array<std::uint8_t, protocol::MaxDatagramSize> datagram{};
        std::size_t written = 0;
        const auto result = protocol::encode_datagram(header,
            {protocol::VersionMinorV1_1, protocol::VersionMinorV1_1}, bytes(payload),
            {datagram.data(), datagram.size()}, written);
        if (result != protocol::ValidationError::None) return false;
        return m_socket->write(reinterpret_cast<const char*>(datagram.data()),
                               static_cast<qint64>(written)) == static_cast<qint64>(written);
    }

    void beginHello()
    {
        protocol::HelloPayload hello;
        m_nonce = QRandomGenerator::global()->generate64();
        if (m_nonce == 0) m_nonce = 1;
        hello.client_nonce = m_nonce;
        m_helloT0Us = nowUs();
        hello.client_send_t0_us = m_helloT0Us;
        hello.min_major = protocol::VersionMajor;
        hello.max_major = protocol::VersionMajor;
        hello.min_minor = protocol::VersionMinorV1_1;
        hello.max_minor = protocol::VersionMinorV1_1;
        hello.requested_visibility_mode = protocol::VisibilityMode::Cockpit;
        hello.requested_heartbeat_ms = 1000;
        if (!encodePayload(hello, protocol::HelloPayloadPrefixSize,
                           protocol::encode_hello_payload, m_helloPayload)) {
            fail(tr("Encodage HELLO impossible"));
            return;
        }
        m_helloMessageId = ++m_messageId;
        m_helloFirstUs = nowUs();
        sendHello(false);
    }

    void sendHello(bool retransmission)
    {
        const std::uint8_t flags = retransmission
            ? protocol::MessageFlagRetransmission : protocol::MessageFlagNone;
        if (m_helloMessageId == 0 || m_helloPayload.isEmpty() ||
            !sendMessage(protocol::MessageType::Hello, m_helloPayload, flags, 0,
                         m_helloMessageId)) {
            retryEndpoint(tr("Envoi HELLO impossible, nouvelle tentative…"));
            return;
        }
        m_lastHelloUs = nowUs();
    }

    void sendAck(const protocol::TelemetryDatagramHeader& target, std::uint8_t flags)
    {
        protocol::AckPayload ack;
        ack.target_message_id = target.message_id;
        ack.target_message_type = target.message_type;
        ack.ack_flags = flags;
        ack.target_fragment_count = target.fragment_count;
        ack.target_message_crc32 = target.message_crc32;
        QByteArray payload;
        if (encodePayload(ack, protocol::AckPayloadSize, protocol::encode_ack_payload, payload)) {
            sendMessage(protocol::MessageType::Ack, payload);
            m_ackLevels.insert(messageKey(target), flags);
        }
    }

    void sendNack(const protocol::NackPayload& nack)
    {
        QByteArray payload;
        if (encodePayload(nack, protocol::NackPayloadPrefixSize + protocol::MaxNackBitmapBytes,
                          protocol::encode_nack_payload, payload)) {
            sendMessage(protocol::MessageType::Nack, payload);
        }
    }

    void requestResync(protocol::ResyncReason reason)
    {
        if (m_sessionId == 0 || m_resyncPending) return;
        protocol::ResyncRequestPayload request;
        request.request_id = ++m_resyncRequestId;
        if (request.request_id == 0) request.request_id = ++m_resyncRequestId;
        request.reason = reason;
        request.request_flags = protocol::ResyncRequestFlagRequireFullSnapshot;
        request.last_applied_snapshot_id = m_baselineSnapshotId;
        request.last_applied_delta_sequence = m_lastDeltaSequence;
        request.client_send_time_us = nowUs();
        QByteArray payload;
        if (encodePayload(request, protocol::ResyncRequestPayloadSize,
                          protocol::encode_resync_request_payload, payload) &&
            sendMessage(protocol::MessageType::ResyncRequest, payload,
                        protocol::MessageFlagAckRequired)) {
            m_resyncPending = true;
            m_lastResyncUs = nowUs();
            m_resyncPayload = payload;
        }
    }

    void readPendingDatagrams()
    {
        while (m_socket != nullptr && m_socket->hasPendingDatagrams()) {
            QNetworkDatagram networkDatagram = m_socket->receiveDatagram();
            if (networkDatagram.senderAddress() != m_peerAddress ||
                networkDatagram.senderPort() != m_port) continue;
            const QByteArray data = networkDatagram.data();
            protocol::DatagramView fragment;
            // A rejected WELCOME is deliberately encoded with the frozen 1.0
            // header, even when the offer requested 1.1.  Accept both minors
            // while pre-session, then require the negotiated 1.1 minor for
            // every other message.
            const auto validation = protocol::decode_and_validate_datagram(
                bytes(data), {protocol::VersionMinorV1_0, protocol::VersionMinorV1_1}, fragment);
            if (validation != protocol::ValidationError::None) continue;
            if (fragment.header.message_type != protocol::MessageType::Welcome &&
                fragment.header.version_minor != protocol::VersionMinorV1_1) continue;
            if (fragment.header.message_type != protocol::MessageType::Welcome &&
                (m_sessionId == 0 || fragment.header.session_id != m_sessionId)) continue;

            const QString key = messageKey(fragment.header);
            if (m_ackLevels.contains(key)) {
                if ((fragment.header.flags & protocol::MessageFlagAckRequired) != 0U)
                    sendAck(fragment.header, m_ackLevels.value(key));
                continue;
            }

            protocol::ReassembledMessage message;
            const auto pipeline = m_reassembler.ingest_validated_fragment(
                fragment, m_peerEndpoint, nowUs(), 250'000ULL, message);
            if (pipeline.reassembly == protocol::ReassemblyResult::Accepted ||
                pipeline.reassembly == protocol::ReassemblyResult::Duplicate) continue;
            if (pipeline.reassembly != protocol::ReassemblyResult::Completed) {
                if ((fragment.header.flags & protocol::MessageFlagAckRequired) != 0U) {
                    protocol::NackPayload nack;
                    nack.target_message_id = fragment.header.message_id;
                    nack.target_message_type = fragment.header.message_type;
                    nack.target_fragment_count = fragment.header.fragment_count;
                    nack.target_message_crc32 = fragment.header.message_crc32;
                    nack.reason = pipeline.reassembly == protocol::ReassemblyResult::MessageCrcMismatch
                        ? protocol::NackReason::BadMessageCrc
                        : pipeline.reassembly == protocol::ReassemblyResult::QuotaExceeded
                            ? protocol::NackReason::ResourceLimit
                            : protocol::NackReason::BadFragmentLayout;
                    sendNack(nack);
                }
                requestResync(protocol::ResyncReason::ValidationFailed);
                continue;
            }
            processMessage(message.header, message.payload_view());
        }
    }

    void processMessage(const protocol::TelemetryDatagramHeader& header, protocol::ByteView payload)
    {
        switch (header.message_type) {
        case protocol::MessageType::Welcome: processWelcome(header, payload); break;
        case protocol::MessageType::SessionBegin: processSessionBegin(header, payload); break;
        case protocol::MessageType::Manifest: processManifest(header, payload); break;
        case protocol::MessageType::FullSnapshot: processSnapshot(header, payload); break;
        case protocol::MessageType::Delta: processDelta(header, payload); break;
        case protocol::MessageType::Heartbeat: processHeartbeat(header, payload); break;
        case protocol::MessageType::Ack: processAck(payload); break;
        case protocol::MessageType::SessionEnd:
            sendAck(header, protocol::KnownAckFlags);
            openEndpoint(true);
            break;
        default:
            break;
        }
    }

    void processWelcome(const protocol::TelemetryDatagramHeader& header, protocol::ByteView data)
    {
        protocol::WelcomePayload welcome;
        if (protocol::decode_welcome_payload(data, welcome) != protocol::ValidationError::None ||
            welcome.client_nonce != m_nonce || welcome.client_send_t0_us != m_helloT0Us ||
            m_sessionId != 0) {
            fail(tr("WELCOME FSTL 1.1 invalide"));
            return;
        }
        if (welcome.status != protocol::WelcomeStatus::Accepted) {
            if (welcome.status == protocol::WelcomeStatus::Busy) {
                ++m_reconnectGeneration;
                retryEndpoint(tr("Producteur occupé, nouvelle tentative…"));
                return;
            }
            fail(tr("Connexion refusée par le producteur (code %1)")
                     .arg(static_cast<unsigned>(welcome.status)));
            return;
        }
        if (welcome.selected_major != protocol::VersionMajor ||
            welcome.selected_minor != protocol::VersionMinorV1_1 ||
            welcome.selected_visibility_mode != protocol::VisibilityMode::Cockpit ||
            header.session_id == 0) {
            fail(tr("WELCOME FSTL 1.1 invalide"));
            return;
        }
        m_sessionId = header.session_id;
        m_lastProgressUs = nowUs();
        if ((header.flags & protocol::MessageFlagAckRequired) != 0U)
            sendAck(header, protocol::KnownAckFlags);
        emit statusChanged(ClientStatus::Synchronizing, tr("Synchronisation FSTL…"));
    }

    void processSessionBegin(const protocol::TelemetryDatagramHeader& header, protocol::ByteView data)
    {
        protocol::SessionBeginPayload begin;
        if (protocol::decode_session_begin_payload(data, begin) != protocol::ValidationError::None) {
            fail(tr("SESSION_BEGIN invalide"));
            return;
        }
        m_sessionBegun = true;
        m_lastProgressUs = nowUs();
        sendAck(header, protocol::KnownAckFlags);
    }

    void rememberReliable(const protocol::TelemetryDatagramHeader& header)
    {
        m_reliableHeaders.insert(header.message_id, header);
    }

    void applyTransactionAcks(const protocol::CompletedTransaction& completed)
    {
        for (const auto& part : completed.parts) {
            if (m_reliableHeaders.contains(part.message_id)) {
                sendAck(m_reliableHeaders.take(part.message_id), protocol::KnownAckFlags);
            }
        }
    }

    void processManifest(const protocol::TelemetryDatagramHeader& header, protocol::ByteView data)
    {
        protocol::ManifestPartPayload payload;
        if (protocol::decode_manifest_part_payload(data, payload) != protocol::ValidationError::None) {
            fail(tr("MANIFEST invalide"));
            return;
        }
        rememberReliable(header);
        sendAck(header, static_cast<std::uint8_t>(protocol::AckFlag::Validated));
        protocol::CompletedTransaction completed;
        const auto outcome = m_transactions.ingest(transactionPart(payload, header), nowUs() / 1000, completed);
        if (outcome.result == protocol::TransactionAssemblyResult::Completed) {
            for (const auto& part : completed.parts) {
                protocol::RecordEnvelopeIterator iterator(
                    part.records_view(), part.record_count, protocol::RecordFlagPolicy::RequireNone);
                protocol::RecordEnvelopeView record;
                bool hasValue = false;
                for (;;) {
                    if (iterator.next(record, hasValue) != protocol::ValidationError::None) {
                        fail(tr("Transaction MANIFEST invalide"));
                        return;
                    }
                    if (!hasValue) break;
                    protocol::BusinessRecordMetadata metadata;
                    if (protocol::validate_business_record(record,
                            protocol::BusinessRecordContainer::Manifest,
                            protocol::VersionMinorV1_1, metadata) != protocol::ValidationError::None) {
                        fail(tr("Transaction MANIFEST invalide"));
                        return;
                    }
                }
            }
            m_manifestId = completed.transaction_id;
            applyTransactionAcks(completed);
            m_lastProgressUs = nowUs();
        } else if (outcome.result != protocol::TransactionAssemblyResult::Accepted &&
                   outcome.result != protocol::TransactionAssemblyResult::Duplicate) {
            fail(tr("Transaction MANIFEST invalide"));
        }
    }

    bool decodeCompletedSnapshot(const protocol::CompletedTransaction& completed,
                                 protocol::StateImage& output)
    {
        std::size_t byteCount = 0;
        std::uint32_t recordCount = 0;
        for (const auto& part : completed.parts) {
            byteCount += part.records.size();
            recordCount += part.record_count;
        }
        if (recordCount > std::numeric_limits<std::uint16_t>::max()) return false;
        std::vector<std::uint8_t> records;
        records.reserve(byteCount);
        for (const auto& part : completed.parts)
            records.insert(records.end(), part.records.begin(), part.records.end());
        return decodeFstl11SnapshotRegion(
            {records.data(), records.size()}, static_cast<std::uint16_t>(recordCount), output);
    }

    void processSnapshot(const protocol::TelemetryDatagramHeader& header, protocol::ByteView data)
    {
        protocol::FullSnapshotPartPayload payload;
        if (protocol::decode_full_snapshot_part_payload(data, payload) != protocol::ValidationError::None ||
            (payload.required_manifest_id != 0 && payload.required_manifest_id != m_manifestId)) {
            fail(tr("FULL_SNAPSHOT invalide ou manifeste absent"));
            return;
        }
        rememberReliable(header);
        sendAck(header, static_cast<std::uint8_t>(protocol::AckFlag::Validated));
        protocol::CompletedTransaction completed;
        const auto outcome = m_transactions.ingest(transactionPart(payload, header), nowUs() / 1000, completed);
        if (outcome.result == protocol::TransactionAssemblyResult::Completed) {
            protocol::StateImage candidate;
            QString error;
            if (!decodeCompletedSnapshot(completed, candidate)) {
                fail(tr("Snapshot métier invalide"));
                return;
            }
            auto radar = makeRadarImage(candidate, &error);
            if (!radar) {
                fail(error);
                return;
            }
            m_baseline = candidate;
            m_current = candidate;
            m_baselineSnapshotId = completed.transaction_id;
            m_lastDeltaSequence = 0;
            m_hasBaseline = true;
            m_resyncPending = false;
            applyTransactionAcks(completed);
            publish(std::move(radar));
        } else if (outcome.result != protocol::TransactionAssemblyResult::Accepted &&
                   outcome.result != protocol::TransactionAssemblyResult::Duplicate) {
            fail(tr("Transaction FULL_SNAPSHOT invalide"));
        }
    }

    void processDelta(const protocol::TelemetryDatagramHeader&, protocol::ByteView data)
    {
        protocol::DeltaPayload payload;
        protocol::CumulativeStateDelta delta;
        if (!m_hasBaseline || protocol::decode_delta_payload(data, payload) != protocol::ValidationError::None ||
            protocol::decode_business_delta(payload, protocol::VersionMinorV1_1, delta) !=
                protocol::ValidationError::None ||
            payload.baseline_snapshot_id != m_baselineSnapshotId) {
            requestResync(protocol::ResyncReason::UnknownBaseline);
            return;
        }
        if (payload.delta_sequence <= m_lastDeltaSequence) return;
        protocol::StateImage candidate;
        if (protocol::apply_cumulative_state_delta(m_baseline, delta, nullptr, candidate) !=
            protocol::StateDeltaApplyResult::Applied) {
            requestResync(protocol::ResyncReason::ValidationFailed);
            return;
        }
        QString error;
        auto radar = makeRadarImage(candidate, &error);
        if (!radar) {
            fail(error);
            return;
        }
        m_current = candidate;
        m_lastDeltaSequence = payload.delta_sequence;
        publish(std::move(radar));
    }

    void processHeartbeat(const protocol::TelemetryDatagramHeader&, protocol::ByteView data)
    {
        protocol::HeartbeatPayload heartbeat;
        if (protocol::decode_heartbeat_payload(data, heartbeat) != protocol::ValidationError::None) return;
        m_lastProgressUs = nowUs();
        if (heartbeat.kind == protocol::HeartbeatKind::Request) {
            heartbeat.kind = protocol::HeartbeatKind::Response;
            heartbeat.receive_t1_us = nowUs();
            heartbeat.transmit_t2_us = nowUs();
            QByteArray payload;
            if (encodePayload(heartbeat, protocol::HeartbeatPayloadSize,
                              protocol::encode_heartbeat_payload, payload))
                sendMessage(protocol::MessageType::Heartbeat, payload);
        }
    }

    void processAck(protocol::ByteView data)
    {
        protocol::AckPayload ack;
        if (protocol::decode_ack_payload(data, ack) != protocol::ValidationError::None) return;
        if (ack.target_message_type == protocol::MessageType::ResyncRequest) {
            m_resyncPending = false;
        }
    }

    void publish(std::shared_ptr<const RadarImage> image)
    {
        m_lastProgressUs = nowUs();
        m_staleSignalled = false;
        m_resyncPending = false;
        emit imageReady(std::move(image));
        emit statusChanged(ClientStatus::Live, {});
    }

    void onTick()
    {
        const std::uint64_t now = nowUs();
        if (m_sessionId == 0) {
            if (m_socket != nullptr && m_endpointReady && m_helloMessageId != 0 &&
                !m_helloPayload.isEmpty()) {
                if (m_helloFirstUs != 0 && now - m_helloFirstUs >= 10'000'000ULL) {
                    ++m_reconnectGeneration;
                    openEndpoint(true);
                } else if (now - m_lastHelloUs >= 1'000'000ULL) {
                    sendHello(true);
                }
            }
            return;
        }
        // TelemetryReassembler is used for byte assembly; transaction expiry
        // still drives a full resync if a producer stops midway.
        const auto expired = m_transactions.expire_with_details(now / 1000);
        if (expired.count != 0) requestResync(protocol::ResyncReason::ReassemblyTimeout);

        protocol::SelectiveNackAction action;
        while (m_reassembler.poll(now, 0, action) == protocol::SelectiveNackPollResult::Ready) {
            if (action.kind == protocol::SelectiveNackActionKind::SendNack) {
                sendNack(action.nack);
            } else {
                requestResync(protocol::ResyncReason::ReassemblyTimeout);
            }
        }

        if (m_lastProgressUs == 0) return;
        const std::uint64_t silent = now - m_lastProgressUs;
        if (silent >= 10'000'000ULL) {
            ++m_reconnectGeneration;
            openEndpoint(true);
            return;
        }
        if (silent >= 3'000'000ULL && !m_staleSignalled) {
            m_staleSignalled = true;
            emit statusChanged(ClientStatus::Stale, tr("STALE"));
            requestResync(protocol::ResyncReason::SessionStale);
        }
    }

    void fail(const QString& detail)
    {
        m_tick.stop();
        emit statusChanged(ClientStatus::Error, detail);
    }

    QString m_host;
    quint16 m_port = 0;
    QUdpSocket* m_socket = nullptr;
    bool m_endpointReady = false;
    QHostAddress m_peerAddress;
    protocol::EndpointKey m_peerEndpoint;
    QElapsedTimer m_clock;
    QTimer m_tick;
    protocol::ReliableReceivePipeline m_reassembler;
    protocol::TelemetryTransactionAssembler m_transactions;
    protocol::StateImage m_baseline;
    protocol::StateImage m_current;
    QHash<QString, std::uint8_t> m_ackLevels;
    QHash<std::uint32_t, protocol::TelemetryDatagramHeader> m_reliableHeaders;
    std::uint64_t m_sessionId = 0;
    std::uint64_t m_nonce = 0;
    std::uint64_t m_lastProgressUs = 0;
    std::uint64_t m_lastHelloUs = 0;
    std::uint64_t m_helloFirstUs = 0;
    std::uint64_t m_helloT0Us = 0;
    std::uint64_t m_lastResyncUs = 0;
    std::uint64_t m_endpointGeneration = 0;
    std::uint32_t m_sequence = 0;
    std::uint32_t m_messageId = 0;
    std::uint32_t m_helloMessageId = 0;
    std::uint32_t m_manifestId = 0;
    std::uint32_t m_baselineSnapshotId = 0;
    std::uint32_t m_lastDeltaSequence = 0;
    std::uint32_t m_resyncRequestId = 0;
    std::uint32_t m_reconnectGeneration = 0;
    QByteArray m_resyncPayload;
    QByteArray m_helloPayload;
    bool m_hasBaseline = false;
    bool m_sessionBegun = false;
    bool m_staleSignalled = false;
    bool m_resyncPending = false;
};

RadarClient::RadarClient(QObject* parent) : QObject(parent), m_thread(new QThread(this))
{
    qRegisterMetaType<std::shared_ptr<const RadarImage>>();
    qRegisterMetaType<ClientStatus>();
    m_worker = new RadarClientWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &RadarClientWorker::imageReady, this, &RadarClient::imageReady,
            Qt::QueuedConnection);
    connect(m_worker, &RadarClientWorker::statusChanged, this, &RadarClient::statusChanged,
            Qt::QueuedConnection);
    m_thread->start();
}

RadarClient::~RadarClient()
{
    stop();
    m_thread->quit();
    m_thread->wait();
}

void RadarClient::start(const QString& host, quint16 port)
{
    QMetaObject::invokeMethod(m_worker, "startSession", Qt::QueuedConnection,
                              Q_ARG(QString, host), Q_ARG(quint16, port));
}

void RadarClient::stop()
{
    if (m_worker != nullptr)
        QMetaObject::invokeMethod(m_worker, "stopSession", Qt::QueuedConnection);
}

} // namespace simpit::radar

#include "radar_client.moc"

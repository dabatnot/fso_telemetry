#include "radar_client.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/packet_writer.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_replication.h"
#include "telemetry/protocol/telemetry_sha256.h"
#include "telemetry/protocol/telemetry_state_messages.h"

#include <QTest>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace telemetry::protocol;
using namespace simpit::radar;

namespace {

struct CapturedDatagram {
    TelemetryDatagramHeader header;
    QByteArray payload;
    QHostAddress sender;
    quint16 senderPort = 0;
};

bool receiveDecodedMessage(QUdpSocket& socket, MessageType wanted, bool acceptAny,
                           int timeoutMs, CapturedDatagram& captured)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (!socket.hasPendingDatagrams()) {
            QTest::qWait(10);
            continue;
        }
        const QNetworkDatagram network = socket.receiveDatagram();
        const QByteArray wire = network.data();
        DatagramView decoded;
        const ByteView bytes{reinterpret_cast<const std::uint8_t*>(wire.constData()),
                             static_cast<std::size_t>(wire.size())};
        if (decode_and_validate_datagram(
                bytes, SupportedMinorRange, decoded) != ValidationError::None ||
            (!acceptAny && decoded.header.message_type != wanted)) {
            continue;
        }
        captured.header = decoded.header;
        captured.payload = QByteArray(reinterpret_cast<const char*>(decoded.payload.data),
                                      static_cast<qsizetype>(decoded.payload.size));
        captured.sender = network.senderAddress();
        captured.senderPort = network.senderPort();
        return true;
    }
    return false;
}

bool sendMessage(QUdpSocket& socket, const CapturedDatagram& destination,
                 TelemetryDatagramHeader header, ByteView payload)
{
    header.message_size = static_cast<std::uint32_t>(payload.size);
    header.message_crc32 = crc32_iso_hdlc(payload);
    std::array<std::uint8_t, MaxDatagramSize> wire{};
    std::size_t written = 0;
    if (encode_datagram(header, {header.version_minor, header.version_minor}, payload,
                        {wire.data(), wire.size()}, written) != ValidationError::None) {
        return false;
    }
    return socket.writeDatagram(reinterpret_cast<const char*>(wire.data()),
                                static_cast<qint64>(written), destination.sender,
                                destination.senderPort) == static_cast<qint64>(written);
}

bool receiveMessage(QUdpSocket& socket, MessageType wanted, int timeoutMs,
                    CapturedDatagram& captured)
{
    return receiveDecodedMessage(socket, wanted, false, timeoutMs, captured);
}

bool receiveAnyMessage(QUdpSocket& socket, int timeoutMs, CapturedDatagram& captured)
{
    return receiveDecodedMessage(socket, MessageType::Invalid, true, timeoutMs, captured);
}

template <typename Payload, typename Encoder>
QByteArray encodePayload(const Payload& payload, std::size_t capacity, Encoder encoder)
{
    QByteArray encoded(static_cast<qsizetype>(capacity), Qt::Uninitialized);
    std::size_t written = 0;
    if (encoder(payload,
                {reinterpret_cast<std::uint8_t*>(encoded.data()), capacity}, written) !=
        ValidationError::None) {
        return {};
    }
    encoded.resize(static_cast<qsizetype>(written));
    return encoded;
}

std::vector<std::uint8_t> encodeRecord(RecordType type, std::uint8_t version,
                                       ByteView payload,
                                       BusinessRecordContainer container =
                                           BusinessRecordContainer::FullSnapshot)
{
    const RecordEnvelopeView envelope{
        static_cast<std::uint16_t>(type), version, RecordFlagNone, payload};
    std::vector<std::uint8_t> encoded(512);
    std::size_t written = 0;
    if (encode_business_record(envelope, container, VersionMinor,
                               {encoded.data(), encoded.size()}, written) !=
        ValidationError::None) {
        return {};
    }
    encoded.resize(written);
    return encoded;
}

void append(std::vector<std::uint8_t>& destination,
            const std::vector<std::uint8_t>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

std::vector<std::uint8_t> classManifestRecord(std::uint32_t generation)
{
    std::array<std::uint8_t, 256> payload{};
    PacketWriter writer({payload.data(), payload.size()});
    writer.write_u32(generation);
    writer.write_u32(1);
    writer.write_u64(ClassManifestPresenceFlagNone);
    writer.write_utf8("Test Ship", 255);
    writer.write_u32(0);
    writer.write_u32(1);
    writer.write_f32(1.0F);
    writer.write_f32(0.0F);
    writer.write_f32(0.0F);
    writer.write_f32(0.0F);
    if (!writer.ok()) return {};
    return encodeRecord(RecordType::ClassManifest, 1, writer.written(),
                        BusinessRecordContainer::Manifest);
}

std::vector<std::uint8_t> sessionRecord(std::uint64_t observer)
{
    std::array<std::uint8_t, 128> payload{};
    PacketWriter writer({payload.data(), payload.size()});
    writer.write_u64(SessionStatePresenceFlagObservedPlayer);
    writer.write_u64(1);
    writer.write_u64(1234);
    writer.write_u8(static_cast<std::uint8_t>(AuthorityMode::Solo));
    writer.write_u8(static_cast<std::uint8_t>(VisibilityMode::Cockpit));
    writer.write_u8(static_cast<std::uint8_t>(SessionPhase::Live));
    writer.write_u8(0);
    writer.write_u32(1);
    writer.write_u64(0);
    writer.write_u64(0x07cbULL);
    writer.write_u64(0);
    writer.write_u64(0);
    writer.write_u64(observer);
    if (!writer.ok()) return {};
    return encodeRecord(RecordType::SessionState, 1, writer.written());
}

std::vector<std::uint8_t> missionRecord(bool paused)
{
    std::array<std::uint8_t, 64> payload{};
    PacketWriter writer({payload.data(), payload.size()});
    writer.write_u64(0);
    writer.write_u32(1);
    writer.write_u8(static_cast<std::uint8_t>(MissionPhase::Active));
    writer.write_u8(paused ? 1 : 0);
    writer.write_u16(0);
    writer.write_f32(1.0F);
    writer.write_u64(1234);
    if (!writer.ok()) return {};
    return encodeRecord(RecordType::MissionState, 1, writer.written());
}

std::vector<std::uint8_t> lifecycleRecord(std::uint64_t observer)
{
    std::array<std::uint8_t, 64> payload{};
    PacketWriter writer({payload.data(), payload.size()});
    writer.write_u64(observer);
    writer.write_u64(0);
    writer.write_u64(1234);
    writer.write_u8(static_cast<std::uint8_t>(ObjectType::Ship));
    writer.write_u8(static_cast<std::uint8_t>(LifecyclePhase::Active));
    writer.write_u32(0);
    if (!writer.ok()) return {};
    return encodeRecord(RecordType::EntityLifecycle, 1, writer.written());
}

std::vector<std::uint8_t> flightRecord(std::uint64_t observer)
{
    std::array<std::uint8_t, 128> payload{};
    PacketWriter writer({payload.data(), payload.size()});
    writer.write_u64(observer);
    writer.write_u64(0);
    writer.write_u64(1234);
    for (int index = 0; index < 3; ++index) writer.write_f32(0.0F);
    writer.write_f32(1.0F);
    for (int index = 0; index < 3; ++index) writer.write_f32(0.0F);
    for (int index = 0; index < 3; ++index) writer.write_f32(0.0F);
    for (int index = 0; index < 3; ++index) writer.write_f32(0.0F);
    writer.write_f32(0.0F);
    writer.write_u32(0);
    if (!writer.ok()) return {};
    return encodeRecord(RecordType::FlightState, 1, writer.written());
}

std::vector<std::uint8_t> snapshotRecords(std::uint64_t observer, bool paused)
{
    std::vector<std::uint8_t> records;
    append(records, sessionRecord(observer));
    append(records, missionRecord(paused));
    append(records, lifecycleRecord(observer));
    append(records, flightRecord(observer));
    return records;
}

std::shared_ptr<const RadarImage> capturedImage(const QSignalSpy& spy, int index)
{
    return spy.at(index).at(0).value<std::shared_ptr<const RadarImage>>();
}

class ProducerHarness final {
public:
    bool bind()
    {
        return socket.bind(QHostAddress(QHostAddress::LocalHost), 0);
    }

    bool accept(RadarClient& client)
    {
        client.start(QStringLiteral("127.0.0.1"), socket.localPort());
        if (!receiveMessage(socket, MessageType::Hello, 3000, clientEndpoint)) return false;
        HelloPayload hello;
        if (decode_hello_payload(
                {reinterpret_cast<const std::uint8_t*>(clientEndpoint.payload.constData()),
                 static_cast<std::size_t>(clientEndpoint.payload.size())}, hello) !=
            ValidationError::None) {
            return false;
        }
        clientHello = hello;
        WelcomePayload welcome;
        welcome.client_nonce = hello.client_nonce;
        welcome.client_send_t0_us = hello.client_send_t0_us;
        welcome.producer_receive_t1_us = hello.client_send_t0_us + 1;
        welcome.producer_send_t2_us = hello.client_send_t0_us + 2;
        welcome.status = WelcomeStatus::Accepted;
        welcome.selected_major = VersionMajor;
        welcome.selected_minor = VersionMinor;
        welcome.selected_visibility_mode = VisibilityMode::Cockpit;
        welcome.heartbeat_interval_ms = 1000;
        welcome.reliable_reassembly_timeout_ms = ReliableReassemblyTimeoutV1Ms;
        welcome.producer_id = 7;
        const QByteArray payload = encodePayload(
            welcome, WelcomePayloadPrefixSize, encode_welcome_payload);
        return !payload.isEmpty() && send(MessageType::Welcome, payload);
    }

    bool beginSession(std::uint64_t missionId)
    {
        SessionBeginPayload begin;
        begin.session_flags = SessionBeginFlagReadOnly |
            SessionBeginFlagMissionActive | SessionBeginFlagManifestRequired;
        begin.producer_session_start_us = 1;
        begin.mission_instance_id = missionId;
        begin.initial_snapshot_id = nextSnapshotId;
        begin.required_manifest_id = manifestId;
        const QByteArray payload = encodePayload(
            begin, SessionBeginPayloadSize, encode_session_begin_payload);
        return !payload.isEmpty() && send(MessageType::SessionBegin, payload);
    }

    const HelloPayload& hello() const noexcept
    {
        return clientHello;
    }

    bool receiveFromClient(int timeoutMs, CapturedDatagram& captured)
    {
        return receiveAnyMessage(socket, timeoutMs, captured);
    }

    bool receiveFromClient(MessageType wanted, int timeoutMs, CapturedDatagram& captured)
    {
        return receiveMessage(socket, wanted, timeoutMs, captured);
    }

    bool sendHeartbeatRequest()
    {
        HeartbeatPayload heartbeat;
        heartbeat.probe_id = 1;
        heartbeat.kind = HeartbeatKind::Request;
        heartbeat.origin_t0_us = 1;
        const QByteArray payload = encodePayload(
            heartbeat, HeartbeatPayloadSize, encode_heartbeat_payload);
        return !payload.isEmpty() && send(MessageType::Heartbeat, payload);
    }

    bool sendUnknownBaselineDelta()
    {
        return send(MessageType::Delta, {});
    }

    bool sendManifest()
    {
        const auto records = classManifestRecord(manifestId);
        if (records.empty()) return false;
        Sha256Digest digest{};
        if (!sha256({records.data(), records.size()}, digest)) return false;
        ManifestPartPayload manifest;
        manifest.manifest_id = manifestId;
        manifest.part_count = 1;
        manifest.transaction_size = static_cast<std::uint32_t>(records.size());
        manifest.transaction_sha256 = digest;
        manifest.producer_sample_time_us = 1;
        manifest.manifest_kind = ManifestKind::FullRequired;
        manifest.record_count = 1;
        manifest.records = {records.data(), records.size()};
        const QByteArray payload = encodePayload(
            manifest, ManifestPartPayloadPrefixSize + records.size(),
            encode_manifest_part_payload);
        return !payload.isEmpty() && send(MessageType::Manifest, payload);
    }

    bool sendSnapshot(std::uint64_t observer, bool paused)
    {
        const auto records = snapshotRecords(observer, paused);
        return sendSnapshotPart(records, 0, 1, 4, nextSnapshotId++);
    }

    bool sendSnapshotWithId(std::uint64_t observer, bool paused, std::uint32_t snapshotId)
    {
        const auto records = snapshotRecords(observer, paused);
        return sendSnapshotPart(records, 0, 1, 4, snapshotId);
    }

    bool sendInterruptedSnapshot(std::uint64_t observer)
    {
        const auto first = sessionRecord(observer);
        const auto mission = missionRecord(false);
        const auto lifecycle = lifecycleRecord(observer);
        const auto flight = flightRecord(observer);
        std::vector<std::uint8_t> complete;
        append(complete, first);
        append(complete, mission);
        append(complete, lifecycle);
        append(complete, flight);
        std::vector<std::uint8_t> firstPart;
        append(firstPart, first);
        append(firstPart, mission);
        return sendSnapshotPart(firstPart, 0, 2, 2, nextSnapshotId++, &complete);
    }

    bool completeInterruptedSnapshot(std::uint64_t observer)
    {
        const auto session = sessionRecord(observer);
        const auto mission = missionRecord(false);
        const auto lifecycle = lifecycleRecord(observer);
        const auto flight = flightRecord(observer);
        std::vector<std::uint8_t> complete;
        append(complete, session);
        append(complete, mission);
        append(complete, lifecycle);
        append(complete, flight);
        std::vector<std::uint8_t> secondPart;
        append(secondPart, lifecycle);
        append(secondPart, flight);
        return sendSnapshotPart(
            secondPart, 1, 2, 2, nextSnapshotId - 1, &complete);
    }

private:
    bool sendSnapshotPart(const std::vector<std::uint8_t>& records,
                          std::uint16_t partIndex, std::uint16_t partCount,
                          std::uint16_t recordCount, std::uint32_t snapshotId,
                          const std::vector<std::uint8_t>* complete = nullptr)
    {
        const auto& transaction = complete == nullptr ? records : *complete;
        Sha256Digest digest{};
        if (!sha256({transaction.data(), transaction.size()}, digest)) return false;
        FullSnapshotPartPayload snapshot;
        snapshot.snapshot_id = snapshotId;
        snapshot.part_index = partIndex;
        snapshot.part_count = partCount;
        snapshot.transaction_size = static_cast<std::uint32_t>(transaction.size());
        snapshot.transaction_sha256 = digest;
        snapshot.producer_sample_time_us = 2;
        snapshot.required_manifest_id = manifestId;
        snapshot.snapshot_flags = SnapshotFlagInitial;
        snapshot.record_count = recordCount;
        snapshot.records = {records.data(), records.size()};
        const QByteArray payload = encodePayload(
            snapshot, FullSnapshotPartPayloadPrefixSize + records.size(),
            encode_full_snapshot_part_payload);
        currentFrameId = snapshotId;
        return !payload.isEmpty() && send(MessageType::FullSnapshot, payload);
    }

    bool send(MessageType type, const QByteArray& payload)
    {
        TelemetryDatagramHeader header;
        header.version_minor = VersionMinor;
        header.message_type = type;
        const bool reliable = type == MessageType::Welcome ||
            type == MessageType::SessionBegin || type == MessageType::Manifest ||
            type == MessageType::FullSnapshot;
        header.flags = reliable ? MessageFlagAckRequired : MessageFlagNone;
        if (type == MessageType::FullSnapshot) header.flags |= MessageFlagKeyframe;
        header.session_id = sessionId;
        header.packet_sequence = sequence++;
        header.message_id = messageId++;
        if (type == MessageType::FullSnapshot) header.frame_id = currentFrameId;
        return sendMessage(socket, clientEndpoint, header,
                           {reinterpret_cast<const std::uint8_t*>(payload.constData()),
                            static_cast<std::size_t>(payload.size())});
    }

    QUdpSocket socket;
    CapturedDatagram clientEndpoint;
    HelloPayload clientHello;
    std::uint64_t sessionId = 42;
    std::uint32_t sequence = 1;
    std::uint32_t messageId = 1;
    std::uint32_t manifestId = 77;
    std::uint32_t nextSnapshotId = 1;
    std::uint32_t currentFrameId = 0;
};

} // namespace

class RadarClientTests final : public QObject {
    Q_OBJECT
private slots:
    void snapshotUsesCurrentFstl11RecordCatalogue()
    {
        std::vector<std::uint8_t> records;
        const auto u8 = [&records](std::uint8_t value) { records.push_back(value); };
        const auto u16 = [&records](std::uint16_t value) {
            records.push_back(static_cast<std::uint8_t>(value));
            records.push_back(static_cast<std::uint8_t>(value >> 8U));
        };
        const auto u32 = [&records](std::uint32_t value) {
            for (unsigned shift = 0; shift < 32U; shift += 8U)
                records.push_back(static_cast<std::uint8_t>(value >> shift));
        };
        const auto u64 = [&records](std::uint64_t value) {
            for (unsigned shift = 0; shift < 64U; shift += 8U)
                records.push_back(static_cast<std::uint8_t>(value >> shift));
        };

        // ENTITY_LIFECYCLE for observer 1, required as the cascade owner of
        // TARGET_STATE in a canonical StateImage.
        u16(static_cast<std::uint16_t>(RecordType::EntityLifecycle));
        u8(1); u8(RecordFlagNone); u16(30);
        u64(1); u64(0); u64(1); u8(1); u8(1); u32(0);

        // Minimal TARGET_STATE v6: observer 1, no optional fields and no
        // selected target. Version 6 belongs to the current FSTL 1.1 contract.
        u16(static_cast<std::uint16_t>(RecordType::TargetState));
        u8(6); u8(RecordFlagNone); u16(32);
        u64(1); u64(0); u64(1); u64(0);

        StateImage fstl11;
        QVERIFY(decodeFstl11SnapshotRegion(
            {records.data(), records.size()}, 2, fstl11));
        QCOMPARE(fstl11.records().size(), std::size_t{2});
    }

    void negotiatesOnlyFstl11Cockpit()
    {
        HelloPayload hello;
        hello.client_nonce = 42;
        hello.client_send_t0_us = 100;
        hello.min_minor = VersionMinor;
        hello.max_minor = VersionMinor;
        hello.requested_visibility_mode = VisibilityMode::Cockpit;
        hello.requested_heartbeat_ms = 1000;
        std::array<std::uint8_t, HelloPayloadPrefixSize> encoded{};
        std::size_t written = 0;
        QCOMPARE(encode_hello_payload(hello, {encoded.data(), encoded.size()}, written), ValidationError::None);
        HelloPayload decoded;
        QCOMPARE(decode_hello_payload({encoded.data(), written}, decoded), ValidationError::None);
        QCOMPARE(decoded.min_minor, VersionMinor);
        QCOMPARE(decoded.max_minor, VersionMinor);
        QCOMPARE(decoded.requested_visibility_mode, VisibilityMode::Cockpit);
        QCOMPARE(decoded.advertised_capabilities, std::uint64_t{CapabilityNone});
        QCOMPARE(decoded.extension_count, std::uint16_t{0});
    }

    void outboundTrafficIsTransportOnly()
    {
        ProducerHarness producer;
        QVERIFY(producer.bind());
        RadarClient client;
        QVERIFY(producer.accept(client));

        const HelloPayload& hello = producer.hello();
        QCOMPARE(hello.min_major, VersionMajor);
        QCOMPARE(hello.max_major, VersionMajor);
        QCOMPARE(hello.min_minor, VersionMinor);
        QCOMPARE(hello.max_minor, VersionMinor);
        QCOMPARE(hello.requested_visibility_mode, VisibilityMode::Cockpit);
        QCOMPARE(hello.advertised_capabilities, std::uint64_t{CapabilityNone});
        QCOMPARE(hello.extension_count, std::uint16_t{0});

        CapturedDatagram outbound;
        QVERIFY(producer.receiveFromClient(2000, outbound));
        QCOMPARE(outbound.header.message_type, MessageType::Ack);

        QVERIFY(producer.beginSession(100));
        QVERIFY(producer.receiveFromClient(2000, outbound));
        QCOMPARE(outbound.header.message_type, MessageType::Ack);

        QVERIFY(producer.sendHeartbeatRequest());
        QVERIFY(producer.receiveFromClient(2000, outbound));
        QCOMPARE(outbound.header.message_type, MessageType::Heartbeat);

        QVERIFY(producer.sendUnknownBaselineDelta());
        QVERIFY(producer.receiveFromClient(2000, outbound));
        QCOMPARE(outbound.header.message_type, MessageType::ResyncRequest);

        client.stop();
        QTest::qWait(50);
    }

    void sessionEndRehandshakesOnSameEndpointAndKeepsHelloRetriesAlive()
    {
        QUdpSocket producer;
        QVERIFY(producer.bind(QHostAddress(QHostAddress::LocalHost), 0));

        RadarClient client;
        client.start(QStringLiteral("127.0.0.1"), producer.localPort());

        CapturedDatagram firstHello;
        QVERIFY2(receiveMessage(producer, MessageType::Hello, 3000, firstHello),
                 "Initial HELLO not received");
        HelloPayload hello;
        QCOMPARE(decode_hello_payload(
                     {reinterpret_cast<const std::uint8_t*>(firstHello.payload.constData()),
                      static_cast<std::size_t>(firstHello.payload.size())}, hello),
                 ValidationError::None);

        WelcomePayload welcome;
        welcome.client_nonce = hello.client_nonce;
        welcome.client_send_t0_us = hello.client_send_t0_us;
        welcome.producer_receive_t1_us = hello.client_send_t0_us + 1;
        welcome.producer_send_t2_us = hello.client_send_t0_us + 2;
        welcome.status = WelcomeStatus::Accepted;
        welcome.selected_major = VersionMajor;
        welcome.selected_minor = VersionMinor;
        welcome.selected_visibility_mode = VisibilityMode::Cockpit;
        welcome.heartbeat_interval_ms = 1000;
        welcome.reliable_reassembly_timeout_ms = ReliableReassemblyTimeoutV1Ms;
        welcome.producer_id = 7;
        std::array<std::uint8_t, WelcomePayloadPrefixSize> welcomeBytes{};
        std::size_t welcomeWritten = 0;
        QCOMPARE(encode_welcome_payload(welcome,
                     {welcomeBytes.data(), welcomeBytes.size()}, welcomeWritten),
                 ValidationError::None);
        TelemetryDatagramHeader welcomeHeader;
        welcomeHeader.version_minor = VersionMinor;
        welcomeHeader.message_type = MessageType::Welcome;
        welcomeHeader.flags = MessageFlagAckRequired;
        welcomeHeader.session_id = 42;
        welcomeHeader.packet_sequence = 1;
        welcomeHeader.message_id = 1;
        QVERIFY(sendMessage(producer, firstHello, welcomeHeader,
                            {welcomeBytes.data(), welcomeWritten}));
        CapturedDatagram welcomeAck;
        QVERIFY2(receiveMessage(producer, MessageType::Ack, 2000, welcomeAck),
                 "WELCOME was not accepted by the client");

        // SESSION_END exercises the same endpoint-preserving replacement used
        // by the silence timeout, without making the test wait for it.
        std::array<std::uint8_t, SessionEndPayloadSize> endBytes{};
        SessionEndPayload end;
        end.reason = SessionEndReason::Restart;
        end.end_flags = SessionEndFlagReconnectAllowed;
        std::size_t endWritten = 0;
        QCOMPARE(encode_session_end_payload(end, {endBytes.data(), endBytes.size()}, endWritten),
                 ValidationError::None);
        TelemetryDatagramHeader endHeader;
        endHeader.version_minor = VersionMinor;
        endHeader.message_type = MessageType::SessionEnd;
        endHeader.flags = MessageFlagAckRequired;
        endHeader.session_id = 42;
        endHeader.packet_sequence = 2;
        endHeader.message_id = 2;
        QVERIFY(sendMessage(producer, firstHello, endHeader, {endBytes.data(), endWritten}));

        CapturedDatagram reopenedHello;
        QVERIFY2(receiveMessage(producer, MessageType::Hello, 3000, reopenedHello),
                 "No HELLO from the preserved endpoint");
        QVERIFY((reopenedHello.header.flags & MessageFlagRetransmission) == 0);
        QCOMPARE(reopenedHello.sender, firstHello.sender);
        QCOMPARE(reopenedHello.senderPort, firstHello.senderPort);
        HelloPayload replacementHello;
        QCOMPARE(decode_hello_payload(
                     {reinterpret_cast<const std::uint8_t*>(reopenedHello.payload.constData()),
                      static_cast<std::size_t>(reopenedHello.payload.size())}, replacementHello),
                 ValidationError::None);
        QVERIFY(replacementHello.client_nonce != hello.client_nonce);

        // The preserved UDP endpoint can still deliver the previous session's
        // WELCOME after the new HELLO has been sent. It must be ignored rather
        // than turning recovery into a permanent SENSOR LINK FAILURE loop.
        welcomeHeader.packet_sequence = 3;
        welcomeHeader.message_id = 3;
        QVERIFY(sendMessage(producer, reopenedHello, welcomeHeader,
                            {welcomeBytes.data(), welcomeWritten}));

        CapturedDatagram retryHello;
        QVERIFY2(receiveMessage(producer, MessageType::Hello, 2500, retryHello),
                 "HELLO retry timer stopped after endpoint replacement");
        QCOMPARE(retryHello.header.message_id, reopenedHello.header.message_id);
        QVERIFY((retryHello.header.flags & MessageFlagRetransmission) != 0);

        // Crossing the five-second reliable window must roll only the timer,
        // not create another nonce while the producer remains unavailable.
        QTest::qWait(5'200);
        int laterHelloCount = 0;
        while (producer.hasPendingDatagrams()) {
            const QNetworkDatagram network = producer.receiveDatagram();
            const QByteArray wire = network.data();
            DatagramView decoded;
            const ByteView bytes{
                reinterpret_cast<const std::uint8_t*>(wire.constData()),
                static_cast<std::size_t>(wire.size())};
            if (decode_and_validate_datagram(
                    bytes, SupportedMinorRange, decoded) != ValidationError::None ||
                decoded.header.message_type != MessageType::Hello) {
                continue;
            }
            HelloPayload laterHello;
            QCOMPARE(decode_hello_payload(decoded.payload, laterHello), ValidationError::None);
            QCOMPARE(laterHello.client_nonce, replacementHello.client_nonce);
            QCOMPARE(laterHello.client_send_t0_us, replacementHello.client_send_t0_us);
            QCOMPARE(decoded.header.message_id, reopenedHello.header.message_id);
            ++laterHelloCount;
        }
        QVERIFY(laterHelloCount >= 4);

        client.stop();
        QTest::qWait(50);
    }

    void duplicateWelcomeKeepsPrewarmedSessionAliveUntilSessionBegin()
    {
        QUdpSocket producer;
        QVERIFY(producer.bind(QHostAddress(QHostAddress::LocalHost), 0));

        RadarClient client;
        client.start(QStringLiteral("127.0.0.1"), producer.localPort());

        CapturedDatagram helloDatagram;
        QVERIFY(receiveMessage(producer, MessageType::Hello, 3000, helloDatagram));
        HelloPayload hello;
        QCOMPARE(decode_hello_payload(
                     {reinterpret_cast<const std::uint8_t*>(helloDatagram.payload.constData()),
                      static_cast<std::size_t>(helloDatagram.payload.size())}, hello),
                 ValidationError::None);

        WelcomePayload welcome;
        welcome.client_nonce = hello.client_nonce;
        welcome.client_send_t0_us = hello.client_send_t0_us;
        welcome.producer_receive_t1_us = hello.client_send_t0_us + 1;
        welcome.producer_send_t2_us = hello.client_send_t0_us + 2;
        welcome.status = WelcomeStatus::Accepted;
        welcome.selected_major = VersionMajor;
        welcome.selected_minor = VersionMinor;
        welcome.selected_visibility_mode = VisibilityMode::Cockpit;
        welcome.heartbeat_interval_ms = 1000;
        welcome.reliable_reassembly_timeout_ms = ReliableReassemblyTimeoutV1Ms;
        welcome.producer_id = 7;
        std::array<std::uint8_t, WelcomePayloadPrefixSize> welcomeBytes{};
        std::size_t welcomeWritten = 0;
        QCOMPARE(encode_welcome_payload(welcome,
                     {welcomeBytes.data(), welcomeBytes.size()}, welcomeWritten),
                 ValidationError::None);

        TelemetryDatagramHeader welcomeHeader;
        welcomeHeader.version_minor = VersionMinor;
        welcomeHeader.message_type = MessageType::Welcome;
        welcomeHeader.flags = MessageFlagAckRequired;
        welcomeHeader.session_id = 42;
        welcomeHeader.message_id = 1;
        for (std::uint32_t sequence = 1; sequence <= 4; ++sequence) {
            welcomeHeader.packet_sequence = sequence;
            if (sequence != 1) welcomeHeader.flags |= MessageFlagRetransmission;
            QVERIFY(sendMessage(producer, helloDatagram, welcomeHeader,
                                {welcomeBytes.data(), welcomeWritten}));
            CapturedDatagram ack;
            QVERIFY(receiveMessage(producer, MessageType::Ack, 1000, ack));
            QTest::qWait(700);
        }

        SessionBeginPayload begin;
        begin.session_flags = SessionBeginFlagReadOnly |
            SessionBeginFlagMissionActive | SessionBeginFlagManifestRequired;
        begin.producer_session_start_us = 1;
        begin.mission_instance_id = 2;
        begin.initial_snapshot_id = 1;
        begin.required_manifest_id = 1;
        std::array<std::uint8_t, SessionBeginPayloadSize> beginBytes{};
        std::size_t beginWritten = 0;
        QCOMPARE(encode_session_begin_payload(begin,
                     {beginBytes.data(), beginBytes.size()}, beginWritten),
                 ValidationError::None);
        TelemetryDatagramHeader beginHeader;
        beginHeader.version_minor = VersionMinor;
        beginHeader.message_type = MessageType::SessionBegin;
        beginHeader.flags = MessageFlagAckRequired;
        beginHeader.session_id = 42;
        beginHeader.packet_sequence = 5;
        beginHeader.message_id = 2;
        QVERIFY(sendMessage(producer, helloDatagram, beginHeader,
                            {beginBytes.data(), beginWritten}));
        CapturedDatagram beginAck;
        QVERIFY2(receiveMessage(producer, MessageType::Ack, 1000, beginAck),
                 "Client rotated the session despite valid WELCOME retransmissions");

        AckPayload decodedAck;
        QCOMPARE(decode_ack_payload(
                     {reinterpret_cast<const std::uint8_t*>(beginAck.payload.constData()),
                      static_cast<std::size_t>(beginAck.payload.size())}, decodedAck),
                 ValidationError::None);
        QCOMPARE(decodedAck.target_message_type, MessageType::SessionBegin);
        QCOMPARE(decodedAck.target_message_id, beginHeader.message_id);

        client.stop();
        QTest::qWait(50);
    }

    void ackTupleRoundTrips()
    {
        AckPayload ack;
        ack.target_message_id = 9;
        ack.target_message_type = MessageType::FullSnapshot;
        ack.ack_flags = KnownAckFlags;
        ack.target_fragment_count = 3;
        ack.target_message_crc32 = 0x12345678U;
        std::array<std::uint8_t, AckPayloadSize> bytes{};
        std::size_t written = 0;
        QCOMPARE(encode_ack_payload(ack, {bytes.data(), bytes.size()}, written), ValidationError::None);
        AckPayload decoded;
        QCOMPARE(decode_ack_payload({bytes.data(), written}, decoded), ValidationError::None);
        QCOMPARE(decoded.target_message_id, ack.target_message_id);
        QCOMPARE(decoded.ack_flags, KnownAckFlags);
    }

    void fragmentationAcceptsDisorderAndDuplicates()
    {
        std::vector<std::uint8_t> logical(MaxFragmentPayload * 2 + 17, 0x5a);
        TelemetryFragmenter fragmenter;
        QVERIFY(TelemetryFragmenter::create({logical.data(), logical.size()}, MessageSizeClass::State, fragmenter));
        TelemetryReassembler reassembler;
        ReassembledMessage complete;
        for (int raw : {2, 0, 0, 1}) {
            FragmentSlice slice;
            QVERIFY(fragmenter.fragment(static_cast<std::size_t>(raw), slice));
            DatagramView fragment;
            fragment.header.version_minor = VersionMinor;
            fragment.header.message_type = MessageType::FullSnapshot;
            fragment.header.flags = MessageFlagFragmented | MessageFlagAckRequired | MessageFlagKeyframe;
            fragment.header.session_id = 1;
            fragment.header.message_id = 10;
            fragment.header.fragment_index = slice.fragment_index;
            fragment.header.fragment_count = slice.fragment_count;
            fragment.header.fragment_offset = slice.fragment_offset;
            fragment.header.message_size = slice.message_size;
            fragment.header.message_crc32 = slice.message_crc32;
            fragment.header.payload_size = static_cast<std::uint16_t>(slice.payload.size);
            fragment.payload = slice.payload;
            const ReassemblyResult result = reassembler.ingest(fragment, complete);
            if (raw == 1) QCOMPARE(result, ReassemblyResult::Completed);
        }
        QCOMPARE(complete.payload, logical);
    }

    void crcCorruptionIsRejected()
    {
        const std::array<std::uint8_t, 3> payload{1, 2, 3};
        TelemetryDatagramHeader header;
        header.version_minor = VersionMinor;
        header.message_type = MessageType::Heartbeat;
        header.packet_sequence = 1;
        header.message_id = 1;
        header.message_size = payload.size();
        header.message_crc32 = crc32_iso_hdlc({payload.data(), payload.size()});
        std::array<std::uint8_t, MaxDatagramSize> wire{};
        std::size_t written = 0;
        QCOMPARE(encode_datagram(header, SupportedMinorRange,
                                  {payload.data(), payload.size()}, {wire.data(), wire.size()}, written),
                 ValidationError::None);
        wire[written - 1] ^= 0xffU;
        DatagramView decoded;
        QVERIFY(decode_and_validate_datagram({wire.data(), written},
                    SupportedMinorRange, decoded) != ValidationError::None);
    }

    void staleAndReconnectThresholds()
    {
        QCOMPARE(statusForSilence(999, 999, true, true), ClientStatus::Live);
        QCOMPARE(statusForSilence(1'000, 999, true, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(1'999, 1'999, true, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(2'000, 1'999, true, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(2'000, 2'000, true, true), ClientStatus::Reconnecting);
        // Heartbeats keep the transport session alive even when the cockpit
        // image itself has not changed for longer than the reconnect grace.
        QCOMPARE(statusForSilence(20'000, 100, true, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(20'000, 20'000, false, false), ClientStatus::Connecting);
        // Producer progress returns immediately to Live.
        QCOMPARE(statusForSilence(0, 0, true, true), ClientStatus::Live);
    }

    void initialReliableTransactionsKeepTheirFullWindow()
    {
        QCOMPARE(statusForSilence(999, 999, true, false), ClientStatus::Synchronizing);
        QCOMPARE(statusForSilence(1'000, 1'000, true, false), ClientStatus::Synchronizing);
        QCOMPARE(statusForSilence(1'999, 1'999, true, false), ClientStatus::Synchronizing);
        QCOMPARE(statusForSilence(2'000, 2'000, true, false), ClientStatus::Synchronizing);
        QCOMPARE(statusForSilence(4'999, 4'999, true, false), ClientStatus::Synchronizing);
        QCOMPARE(statusForSilence(5'000, 5'000, true, false), ClientStatus::Reconnecting);
    }

    void continuityAcrossWaitingPauseRecoveryMissionAndObserverReplacement()
    {
        ProducerHarness producer;
        QVERIFY(producer.bind());
        RadarClient client;
        QSignalSpy statuses(&client, &RadarClient::statusChanged);
        QSignalSpy images(&client, &RadarClient::imageReady);

        QVERIFY(producer.accept(client));
        QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(statuses.cbegin(), statuses.cend(),
            [](const QList<QVariant>& emission) {
                return emission.at(0).value<ClientStatus>() == ClientStatus::Ready;
            }), 2000);
        QCOMPARE(images.count(), 0);

        QVERIFY(producer.beginSession(100));
        QVERIFY(producer.sendManifest());
        QVERIFY(producer.sendInterruptedSnapshot(1));
        QTest::qWait(150);
        QCOMPARE(images.count(), 0);

        QVERIFY(producer.completeInterruptedSnapshot(1));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 1, 2000);
        QCOMPARE(capturedImage(images, 0)->playerEntityId, std::uint64_t{1});
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(statuses.cbegin(), statuses.cend(),
            [](const QList<QVariant>& emission) {
                return emission.at(0).value<ClientStatus>() == ClientStatus::Live;
            }), 2000);

        QVERIFY(producer.sendSnapshot(1, true));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 2, 2000);
        const auto paused = capturedImage(images, 1);
        QVERIFY(paused->missionPaused);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(statuses.cbegin(), statuses.cend(),
            [](const QList<QVariant>& emission) {
                return emission.at(0).value<ClientStatus>() == ClientStatus::Paused;
            }), 2000);
        QTest::qWait(150);
        QCOMPARE(images.count(), 2);
        QCOMPARE(capturedImage(images, 1), paused);

        QVERIFY(producer.sendInterruptedSnapshot(1));
        QTest::qWait(150);
        QCOMPARE(images.count(), 2);

        QVERIFY(producer.beginSession(200));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 3, 2000);
        QVERIFY(!capturedImage(images, 2));

        QVERIFY(producer.sendSnapshot(2, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 4, 2000);
        QCOMPARE(capturedImage(images, 3)->playerEntityId, std::uint64_t{2});
        QVERIFY(!capturedImage(images, 3)->missionPaused);

        QVERIFY(producer.sendSnapshot(3, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 5, 2000);
        QCOMPARE(capturedImage(images, 4)->playerEntityId, std::uint64_t{3});

        client.stop();
        QTest::qWait(50);
    }

    void previousMissionSnapshotCannotReplaceTheNewBaseline()
    {
        ProducerHarness producer;
        QVERIFY(producer.bind());
        RadarClient client;
        QSignalSpy images(&client, &RadarClient::imageReady);

        QVERIFY(producer.accept(client));
        QVERIFY(producer.beginSession(100));
        QVERIFY(producer.sendManifest());
        QVERIFY(producer.sendSnapshot(1, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 1, 2000);

        QVERIFY(producer.beginSession(200));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 2, 2000);
        QVERIFY(!capturedImage(images, 1));

        // Snapshot 1 belongs to the previous mission; the new SESSION_BEGIN
        // announced snapshot 2 as its only acceptable initial baseline.
        QVERIFY(producer.sendSnapshotWithId(99, false, 1));
        QTest::qWait(150);
        QCOMPARE(images.count(), 2);

        QVERIFY(producer.sendSnapshot(2, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 3, 2000);
        QCOMPARE(capturedImage(images, 2)->playerEntityId, std::uint64_t{2});

        client.stop();
        QTest::qWait(50);
    }

    void resyncReplacesAnInterruptedSnapshotCandidate()
    {
        ProducerHarness producer;
        QVERIFY(producer.bind());
        RadarClient client;
        QSignalSpy statuses(&client, &RadarClient::statusChanged);
        QSignalSpy images(&client, &RadarClient::imageReady);

        QVERIFY(producer.accept(client));
        QVERIFY(producer.beginSession(100));
        QVERIFY(producer.sendManifest());
        QVERIFY(producer.sendSnapshot(1, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 1, 2000);

        QVERIFY(producer.sendInterruptedSnapshot(2));
        CapturedDatagram resync;
        QVERIFY(producer.receiveFromClient(MessageType::ResyncRequest, 2000, resync));

        // The producer intentionally answers with a new transaction rather
        // than completing the abandoned one.
        QVERIFY(producer.sendSnapshot(3, false));
        QTRY_COMPARE_WITH_TIMEOUT(images.count(), 2, 2000);
        QCOMPARE(capturedImage(images, 1)->playerEntityId, std::uint64_t{3});
        QVERIFY(std::none_of(statuses.cbegin(), statuses.cend(),
            [](const QList<QVariant>& emission) {
                return emission.at(0).value<ClientStatus>() == ClientStatus::Error;
            }));

        client.stop();
        QTest::qWait(50);
    }

    void atomicDeltaDeletion()
    {
        StateAtom atom;
        atom.key.record_type = 900;
        atom.key.identity = {1};
        atom.lifecycle = StateRecordLifecycle::ExplicitCreateDelete;
        atom.value = {1, 7};
        StateImage baseline;
        QCOMPARE(StateImage::create({atom}, baseline), StateImageResult::Created);
        StateMutation deletion;
        deletion.kind = StateMutationKind::Delete;
        deletion.atom = atom;
        deletion.atom.value.clear();
        CumulativeStateDelta delta;
        delta.baseline_snapshot_id = 1;
        delta.delta_sequence = 1;
        delta.mutations = {deletion};
        StateImage applied;
        QCOMPARE(apply_cumulative_state_delta(baseline, delta, nullptr, applied),
                 StateDeltaApplyResult::Applied);
        QVERIFY(applied.empty());
    }
};

QTEST_GUILESS_MAIN(RadarClientTests)
#include "radar_client_tests.moc"

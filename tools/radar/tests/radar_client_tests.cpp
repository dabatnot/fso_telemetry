#include "radar_client.h"

#include "telemetry/protocol/telemetry_control_messages.h"
#include "telemetry/protocol/telemetry_business_records.h"
#include "telemetry/protocol/telemetry_crc32.h"
#include "telemetry/protocol/telemetry_datagram.h"
#include "telemetry/protocol/telemetry_fragmenter.h"
#include "telemetry/protocol/telemetry_reassembler.h"
#include "telemetry/protocol/telemetry_reliability_messages.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <QTest>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QUdpSocket>

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

bool receiveMessage(QUdpSocket& socket, MessageType wanted, int timeoutMs,
                    CapturedDatagram& captured)
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
                bytes, {VersionMinorV1_0, VersionMinorV1_1}, decoded) != ValidationError::None ||
            decoded.header.message_type != wanted) {
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

} // namespace

class RadarClientTests final : public QObject {
    Q_OBJECT
private slots:
    void snapshotUsesNegotiatedFstl11RecordCatalogue()
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
        // selected target. Version 6 is legal only after FSTL 1.1 negotiation.
        u16(static_cast<std::uint16_t>(RecordType::TargetState));
        u8(6); u8(RecordFlagNone); u16(32);
        u64(1); u64(0); u64(1); u64(0);

        StateImage legacy;
        QCOMPARE(decode_business_snapshot_region(
                     {records.data(), records.size()}, 2, legacy),
                 ValidationError::UnsupportedRecordVersion);

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
        hello.min_minor = VersionMinorV1_1;
        hello.max_minor = VersionMinorV1_1;
        hello.requested_visibility_mode = VisibilityMode::Cockpit;
        hello.requested_heartbeat_ms = 1000;
        std::array<std::uint8_t, HelloPayloadPrefixSize> encoded{};
        std::size_t written = 0;
        QCOMPARE(encode_hello_payload(hello, {encoded.data(), encoded.size()}, written), ValidationError::None);
        HelloPayload decoded;
        QCOMPARE(decode_hello_payload({encoded.data(), written}, decoded), ValidationError::None);
        QCOMPARE(decoded.min_minor, VersionMinorV1_1);
        QCOMPARE(decoded.max_minor, VersionMinorV1_1);
        QCOMPARE(decoded.requested_visibility_mode, VisibilityMode::Cockpit);
    }

    void sessionEndReopensEndpointAndKeepsHelloRetriesAlive()
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
        welcome.selected_minor = VersionMinorV1_1;
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
        welcomeHeader.version_minor = VersionMinorV1_1;
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

        // SESSION_END exercises the same endpoint replacement used by the
        // ten-second silence timeout, without making the test wait ten seconds.
        std::array<std::uint8_t, SessionEndPayloadSize> endBytes{};
        SessionEndPayload end;
        end.reason = SessionEndReason::Restart;
        end.end_flags = SessionEndFlagReconnectAllowed;
        std::size_t endWritten = 0;
        QCOMPARE(encode_session_end_payload(end, {endBytes.data(), endBytes.size()}, endWritten),
                 ValidationError::None);
        TelemetryDatagramHeader endHeader;
        endHeader.version_minor = VersionMinorV1_1;
        endHeader.message_type = MessageType::SessionEnd;
        endHeader.flags = MessageFlagAckRequired;
        endHeader.session_id = 42;
        endHeader.packet_sequence = 2;
        endHeader.message_id = 2;
        QVERIFY(sendMessage(producer, firstHello, endHeader, {endBytes.data(), endWritten}));

        CapturedDatagram reopenedHello;
        QVERIFY2(receiveMessage(producer, MessageType::Hello, 3000, reopenedHello),
                 "No HELLO from the reopened endpoint");
        QVERIFY((reopenedHello.header.flags & MessageFlagRetransmission) == 0);

        CapturedDatagram retryHello;
        QVERIFY2(receiveMessage(producer, MessageType::Hello, 2500, retryHello),
                 "HELLO retry timer stopped after endpoint replacement");
        QCOMPARE(retryHello.header.message_id, reopenedHello.header.message_id);
        QVERIFY((retryHello.header.flags & MessageFlagRetransmission) != 0);

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
            fragment.header.version_minor = VersionMinorV1_1;
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
        header.version_minor = VersionMinorV1_1;
        header.message_type = MessageType::Heartbeat;
        header.packet_sequence = 1;
        header.message_id = 1;
        header.message_size = payload.size();
        header.message_crc32 = crc32_iso_hdlc({payload.data(), payload.size()});
        std::array<std::uint8_t, MaxDatagramSize> wire{};
        std::size_t written = 0;
        QCOMPARE(encode_datagram(header, {VersionMinorV1_1, VersionMinorV1_1},
                                  {payload.data(), payload.size()}, {wire.data(), wire.size()}, written),
                 ValidationError::None);
        wire[written - 1] ^= 0xffU;
        DatagramView decoded;
        QVERIFY(decode_and_validate_datagram({wire.data(), written},
                    {VersionMinorV1_1, VersionMinorV1_1}, decoded) != ValidationError::None);
    }

    void staleAndReconnectThresholds()
    {
        QCOMPARE(statusForSilence(2'999, true), ClientStatus::Live);
        QCOMPARE(statusForSilence(3'000, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(9'999, true), ClientStatus::Stale);
        QCOMPARE(statusForSilence(10'000, true), ClientStatus::Reconnecting);
        QCOMPARE(statusForSilence(20'000, false), ClientStatus::Connecting);
        // Producer progress returns immediately to Live.
        QCOMPARE(statusForSilence(0, true), ClientStatus::Live);
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

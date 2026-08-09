from __future__ import annotations

import socket
import struct
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from dashboard_runtime import TelemetryRuntime
from replay_udp import (
    CAPTURED_FRAGMENT_LIMIT,
    PEER_IDLE_TIMEOUT_US,
    ReplayPeer,
    ReplayUdpProducer,
)
import fstl_client_core as fstl
import fstl_reference_decoder as decoder
import test_fstl_console_client_contract as contract


def populated_client() -> fstl.ConsoleClient:
    client = fstl.ConsoleClient(None, 3_000_000)
    nonce, t0 = 10, 20
    client.state.hello_sent = True
    client.state.hello_nonce = nonce
    client.state.hello_t0_us = t0
    hello = fstl.pack_header(
        message_type=2,
        flags=0,
        session_id=0,
        sequence=1,
        sent_us=t0,
        message_id=1,
        payload=fstl.hello_payload(nonce, t0),
    )
    session = 0x12345678
    client.receive(
        contract.packet(3, contract.welcome_for(hello), session_id=session, sequence=1, sent_us=100, flags=2),
        100,
        "1970-01-01T00:00:00.000100Z",
    )
    client.receive(
        contract.packet(4, contract.session_begin_payload(), session_id=session, sequence=2, sent_us=101, flags=2),
        101,
        "1970-01-01T00:00:00.000101Z",
    )
    client.receive(
        contract.packet(6, contract.v11_payload("minimal-with-player", ".bin"), session_id=session, sequence=3, sent_us=102, flags=6),
        102,
        "1970-01-01T00:00:00.000102Z",
    )
    return client


class ReplayUdpProducerTest(unittest.TestCase):
    @staticmethod
    def negotiate(port: int) -> tuple[socket.socket, fstl.ConsoleClient]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("127.0.0.1", port))
        sock.settimeout(0.1)
        client = fstl.ConsoleClient(sock, 3_000_000)
        client.begin()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and client.state.status != "Live":
            try:
                datagram = sock.recv(1200)
            except socket.timeout:
                continue
            at_us, at_utc = fstl.local_observation()
            client.receive(datagram, at_us, at_utc)
        return sock, client

    def test_two_loopback_clients_negotiate_independent_sessions_at_cursor(self) -> None:
        source = populated_client()
        changes: list[dict[str, object]] = []
        producer = ReplayUdpProducer(lambda: source.state, lambda: 550_000, changes.append)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        endpoint = producer.start()["endpoint"]
        self.assertIsNotNone(endpoint)
        port = int(str(endpoint).rsplit(":", 1)[1])
        first_sock, first = self.negotiate(port)
        second_sock, second = self.negotiate(port)
        with first_sock, second_sock:
            self.assertEqual("Live", first.state.status)
            self.assertEqual("Live", second.state.status)
            self.assertNotEqual(first.state.session_id, second.state.session_id)
            self.assertEqual(source.state.records["SESSION_STATE"], first.state.records["SESSION_STATE"])
            self.assertEqual(source.state.records["SESSION_STATE"], second.state.records["SESSION_STATE"])
            self.assertEqual(2, producer.snapshot()["clientCount"])
        producer.stop()

    def test_runtime_rejects_non_unit_replay_speed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
            )
            runtime.mode = "replay"
            with self.assertRaisesRegex(ValueError, "fixed at 1.0"):
                runtime.replay_control(speed=2.0)

    def test_fifth_client_is_refused_without_disturbing_existing_sessions(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        accepted: list[socket.socket] = []
        try:
            for _ in range(4):
                sock, client = self.negotiate(port)
                self.assertEqual("Live", client.state.status)
                accepted.append(sock)
            fifth = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                fifth.connect(("127.0.0.1", port))
                fifth.settimeout(0.5)
                fstl.ConsoleClient(fifth, 3_000_000).begin()
                fifth.recv(1200)
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline and producer.snapshot()["refusedClients"] == 0:
                    time.sleep(0.01)
                self.assertEqual(4, producer.snapshot()["clientCount"])
                self.assertGreaterEqual(producer.snapshot()["refusedClients"], 1)
            finally:
                fifth.close()
        finally:
            for sock in accepted:
                sock.close()
            producer.stop()

    def test_hello_without_fstl_1_1_is_rejected_before_allocating_peer(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("127.0.0.1", port))
            sock.settimeout(1.0)
            nonce, sent_us = 91, 92
            hello = struct.pack(
                "<QQBBBBB3xQHHH",
                nonce,
                sent_us,
                1,
                1,
                0,
                0,
                0,
                0,
                1000,
                0,
                0,
            )
            sock.send(
                fstl.pack_header(
                    message_type=2,
                    flags=0,
                    session_id=0,
                    sequence=1,
                    sent_us=sent_us,
                    message_id=1,
                    payload=hello,
                    minor=0,
                )
            )
            response = sock.recv(1200)

        header = decoder.read_header(response)
        fields = decoder.decode_message(
            3,
            int(header["flags"]),
            response[fstl.HEADER_SIZE :],
            {"senderRole": "producer", "allowedSenderRoles": ["producer"]},
        )["fields"]
        self.assertEqual(0, header["version_minor"])
        self.assertEqual(1, fields["status"])
        self.assertEqual(0, fields["selected_major"])
        self.assertEqual(0, fields["selected_minor"])
        self.assertEqual(0, fields["heartbeat_interval_ms"])
        self.assertEqual(0, producer.snapshot()["clientCount"])
        producer.stop()

    def test_corrupt_client_datagrams_never_allocate_a_peer(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        nonce, sent_us = 101, 202
        hello = bytearray(fstl.pack_header(
            message_type=2,
            flags=0,
            session_id=0,
            sequence=1,
            sent_us=sent_us,
            message_id=1,
            payload=fstl.hello_payload(nonce, sent_us),
        ))

        bad_magic = bytearray(hello)
        bad_magic[0] ^= 0xFF
        bad_crc = bytearray(hello)
        bad_crc[-1] ^= 0xFF
        for datagram in (bad_magic, bad_crc):
            producer._receive(bytes(datagram), ("127.0.0.1", 50000))

        self.assertEqual(0, producer.snapshot()["clientCount"])

    def test_virtual_wire_clock_advances_while_playing_and_freezes_on_pause(self) -> None:
        source = populated_client()
        position_us = [1_000_000]
        playing = [True]
        with mock.patch("replay_udp.fstl.now_us", return_value=5_000_000):
            producer = ReplayUdpProducer(
                lambda: source.state,
                lambda: position_us[0],
                lambda _: None,
                lambda: playing[0],
            )
        peer = ReplayPeer(("127.0.0.1", 50000), 1, clock_offset_us=4_000_000)

        with mock.patch("replay_udp.fstl.now_us", return_value=5_400_000):
            self.assertEqual(5_400_000, producer._wire_time(peer))
            position_us[0] = 1_400_000
            playing[0] = False
            producer.synchronize_clock()
        with mock.patch("replay_udp.fstl.now_us", return_value=6_000_000):
            self.assertEqual(5_400_000, producer._wire_time(peer))

    def test_inactive_client_slot_expires_and_accepts_a_replacement(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        sock, client = self.negotiate(port)
        self.assertEqual("Live", client.state.status)
        sock.close()
        with producer._lock:
            last_received_us = next(iter(producer._peers.values())).last_received_us

        producer._poll(last_received_us + PEER_IDLE_TIMEOUT_US)
        self.assertEqual(0, producer.snapshot()["clientCount"])

        replacement_sock, replacement = self.negotiate(port)
        try:
            self.assertEqual("Live", replacement.state.status)
            self.assertEqual(1, producer.snapshot()["clientCount"])
        finally:
            replacement_sock.close()
            producer.stop()

    def test_client_resync_receives_a_new_synthetic_baseline(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        sock, client = self.negotiate(port)
        try:
            original = client.state.baseline
            client._resync(fstl.now_us())
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline and client.state.baseline == original:
                try:
                    datagram = sock.recv(1200)
                except socket.timeout:
                    continue
                at_us, at_utc = fstl.local_observation()
                client.receive(datagram, at_us, at_utc)
            self.assertGreater(client.state.baseline, original)
            self.assertEqual("Live", client.state.status)
        finally:
            sock.close()
            producer.stop()

    def test_seek_restart_ends_old_session_and_allows_fresh_negotiation(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        sock, client = self.negotiate(port)
        try:
            old_session = client.state.session_id
            producer.restart_sessions()
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline and client.state.status != "Disconnected":
                try:
                    datagram = sock.recv(1200)
                except socket.timeout:
                    continue
                at_us, at_utc = fstl.local_observation()
                client.receive(datagram, at_us, at_utc)
            self.assertEqual("Disconnected", client.state.status)
            replacement = fstl.ConsoleClient(sock, 3_000_000)
            replacement.begin()
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline and replacement.state.status != "Live":
                try:
                    datagram = sock.recv(1200)
                except socket.timeout:
                    continue
                at_us, at_utc = fstl.local_observation()
                replacement.receive(datagram, at_us, at_utc)
            self.assertEqual("Live", replacement.state.status)
            self.assertNotEqual(old_session, replacement.state.session_id)
        finally:
            sock.close()
            producer.stop()

    def test_incomplete_source_fragments_are_ignored_without_peers_and_bounded_with_peer(self) -> None:
        source = populated_client()
        producer = ReplayUdpProducer(lambda: source.state, lambda: 0, lambda _: None)
        producer.observe_capture_datagram(contract.incomplete_fragment(1))
        self.assertEqual({}, producer._captured)

        producer.configure(bind_host="127.0.0.1", port=0, lan_enabled=False)
        port = int(str(producer.start()["endpoint"]).rsplit(":", 1)[1])
        sock, client = self.negotiate(port)
        try:
            self.assertEqual("Live", client.state.status)
            for message_id in range(1, CAPTURED_FRAGMENT_LIMIT + 8):
                producer.observe_capture_datagram(contract.incomplete_fragment(message_id))
            self.assertEqual(CAPTURED_FRAGMENT_LIMIT, len(producer._captured))
            self.assertNotIn((1, 1), producer._captured)
        finally:
            sock.close()
            producer.stop()


if __name__ == "__main__":
    unittest.main()

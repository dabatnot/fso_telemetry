from __future__ import annotations

import base64
import json
import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from dashboard_runtime import (
    CAPTURE_SCHEMA,
    LEGACY_CAPTURE_SCHEMA,
    CaptureWriter,
    ChannelMeasurement,
    QualityTracker,
    TelemetryRuntime,
    configure_live_udp_socket,
    load_capture,
)
import fstl_client_core as fstl

import test_fstl_console_client_contract as contract


class DashboardRuntimeTest(unittest.TestCase):
    @staticmethod
    def wait_for(predicate, timeout: float = 2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.01)
        return None

    def test_channel_measurement_distinguishes_zero_age_and_detects_gap(self) -> None:
        channel = ChannelMeasurement("FLIGHT_STATE", "FLIGHT_STATE/entity_id=1", 30.0)
        channel.observe(0, 0)
        channel.observe(33_334, 33_334)
        channel.observe(100_002, 100_002)
        snapshot = channel.snapshot(0)
        self.assertEqual(0, snapshot["ageUs"])
        self.assertEqual(1, snapshot["gapCount"])
        self.assertAlmostEqual(19.9996, snapshot["observedHz"], places=3)

    def test_windows_udp_port_unreachable_is_configured_as_silence(self) -> None:
        if sys.platform != "win32":
            self.skipTest("Windows Winsock behavior")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            configure_live_udp_socket(client)

    def test_repeated_producer_sample_is_not_an_update(self) -> None:
        channel = ChannelMeasurement("ENERGY_STATE", "ENERGY_STATE/entity_id=1", 10.0)
        channel.observe(1_000, 2_000)
        channel.observe(1_000, 3_000)
        self.assertEqual(1, channel.updates)
        self.assertEqual(1, channel.repeated_samples)

    def test_two_minutes_at_sixty_hz_keep_measurement_memory_bounded(self) -> None:
        channel = ChannelMeasurement("FLIGHT_STATE", "FLIGHT_STATE/entity_id=1", 60.0)
        for index in range(120 * 60):
            timestamp = index * 16_667
            channel.observe(timestamp, timestamp)
        self.assertEqual(120 * 60, channel.updates)
        self.assertEqual(512, len(channel.samples_us))
        self.assertEqual(512, len(channel.received_us))
        self.assertAlmostEqual(60.0, channel.snapshot(0)["observedHz"], places=1)

    def test_capture_round_trip_preserves_datagram_and_zero_timestamp(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            writer = CaptureWriter(Path(directory))
            path = writer.start({"flightHz": 30})
            writer.packet(b"\x00fstl\xff", 0, "1970-01-01T00:00:00.000000Z")
            writer.stop()
            header, packets = load_capture(path)
            self.assertEqual(CAPTURE_SCHEMA, header["schema"])
            self.assertEqual(0, packets[0]["receivedMonotonicUs"])
            self.assertEqual(b"\x00fstl\xff", packets[0]["datagram"])

    def test_capture_loader_fails_closed_on_unknown_schema(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.jsonl"
            path.write_text(json.dumps({"schema": "other", "kind": "header"}) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "capture header"):
                load_capture(path)

    def test_capture_loader_accepts_only_the_explicit_legacy_schema(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "legacy.jsonl"
            path.write_text(
                json.dumps({"schema": LEGACY_CAPTURE_SCHEMA, "kind": "header"}) + "\n",
                encoding="utf-8",
            )
            header, packets = load_capture(path)
            self.assertEqual(LEGACY_CAPTURE_SCHEMA, header["schema"])
            self.assertEqual([], packets)

    def test_empty_snapshot_exposes_five_state_contract_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
            )
            snapshot = runtime.latest()
            self.assertEqual("DashboardSnapshotV1", snapshot["schema"])
            self.assertEqual("Disconnected", snapshot["connection"]["status"])
            self.assertEqual("idle", snapshot["connection"]["recoveryState"])
            self.assertFalse(snapshot["transport"]["synchronized"])
            self.assertEqual([], snapshot["quality"]["channels"])

    def test_suspended_producer_resumes_same_session_without_reloading_dashboard(self) -> None:
        with tempfile.TemporaryDirectory() as directory, socket.socket(
            socket.AF_INET, socket.SOCK_DGRAM
        ) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=server.getsockname()[1],
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
                stale_us=50_000,
            )
            runtime.start()
            try:
                hello, client_address = server.recvfrom(1200)
                first_session = 0x1122334455667788
                server.sendto(
                    contract.packet(
                        3,
                        contract.welcome_for(hello),
                        session_id=first_session,
                        sequence=1,
                        sent_us=1_000_000,
                        flags=2,
                    ),
                    client_address,
                )
                server.sendto(
                    contract.packet(
                        4,
                        contract.session_begin_payload(),
                        session_id=first_session,
                        sequence=2,
                        sent_us=1_000_001,
                        flags=2,
                    ),
                    client_address,
                )
                server.sendto(
                    contract.packet(
                        6,
                        contract.v11_payload("minimal-with-player", ".bin"),
                        session_id=first_session,
                        sequence=3,
                        sent_us=1_000_002,
                        flags=2,
                    ),
                    client_address,
                )
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.latest()["connection"]["status"] == "Live"
                    )
                )
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.latest()["connection"]["status"] == "Stale"
                    )
                )
                self.assertEqual("resyncing", runtime.latest()["connection"]["recoveryState"])

                resync_request = None
                resync_address = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet_bytes, address = server.recvfrom(1200)
                    if contract.reference.read_header(packet_bytes)["message_type"] == 12:
                        resync_request, resync_address = packet_bytes, address
                        break
                self.assertIsNotNone(resync_request)
                self.assertEqual(
                    client_address,
                    resync_address,
                    "an intentional pause must preserve the active UDP endpoint",
                )
                self.assertEqual("Stale", runtime.latest()["connection"]["status"])

                for part_index, payload in enumerate(contract.snapshot_parts(2)):
                    server.sendto(
                        contract.packet(
                            6,
                            payload,
                            session_id=first_session,
                            sequence=10 + part_index,
                            sent_us=2_000_000 + part_index,
                            flags=2,
                        ),
                        client_address,
                    )
                recovered = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["connection"]["status"] == "Live"
                        and snapshot["connection"]["sessionId"] == str(first_session)
                        and snapshot["transport"]["baseline"] == 2
                        else None
                    )
                )
                self.assertIsNotNone(recovered)
                self.assertEqual("idle", recovered["connection"]["recoveryState"])
            finally:
                runtime.stop()

    def test_stuck_stale_session_rotates_endpoint_without_erasing_last_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory, socket.socket(
            socket.AF_INET, socket.SOCK_DGRAM
        ) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=server.getsockname()[1],
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
                stale_us=50_000,
                recovery_reconnect_us=150_000,
            )
            runtime.start()
            try:
                hello, first_address = server.recvfrom(1200)
                first_session = 0x1122334455667788
                server.sendto(
                    contract.packet(
                        3,
                        contract.welcome_for(hello),
                        session_id=first_session,
                        sequence=1,
                        sent_us=1_000_000,
                        flags=2,
                    ),
                    first_address,
                )
                server.sendto(
                    contract.packet(
                        4,
                        contract.session_begin_payload(),
                        session_id=first_session,
                        sequence=2,
                        sent_us=1_000_001,
                        flags=2,
                    ),
                    first_address,
                )
                server.sendto(
                    contract.packet(
                        6,
                        contract.v11_payload("minimal-with-player", ".bin"),
                        session_id=first_session,
                        sequence=3,
                        sent_us=1_000_002,
                        flags=2,
                    ),
                    first_address,
                )
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.latest()["connection"]["status"] == "Live"
                    )
                )
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.latest()["connection"]["status"] == "Stale"
                    )
                )

                second_hello = None
                second_address = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet_bytes, address = server.recvfrom(1200)
                    if (
                        contract.reference.read_header(packet_bytes)["message_type"] == 2
                        and address != first_address
                    ):
                        second_hello, second_address = packet_bytes, address
                        break
                self.assertIsNotNone(second_hello)
                self.assertEqual("reconnecting", runtime.latest()["connection"]["recoveryState"])
                self.assertEqual("Disconnected", runtime.latest()["connection"]["status"])
                self.assertEqual("0", runtime.latest()["connection"]["sessionId"])

                second_session = 0x8877665544332211
                server.sendto(
                    contract.packet(
                        3,
                        contract.welcome_for(second_hello),
                        session_id=second_session,
                        sequence=10,
                        sent_us=2_000_000,
                        flags=2,
                    ),
                    second_address,
                )
                server.sendto(
                    contract.packet(
                        4,
                        contract.session_begin_payload(),
                        session_id=second_session,
                        sequence=11,
                        sent_us=2_000_001,
                        flags=2,
                    ),
                    second_address,
                )
                server.sendto(
                    contract.packet(
                        6,
                        contract.v11_payload("minimal-with-player", ".bin"),
                        session_id=second_session,
                        sequence=12,
                        sent_us=2_000_002,
                        flags=2,
                    ),
                    second_address,
                )
                recovered = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["connection"]["status"] == "Live"
                        and snapshot["connection"]["sessionId"] == str(second_session)
                        else None
                    )
                )
                self.assertIsNotNone(recovered)
                self.assertEqual("idle", recovered["connection"]["recoveryState"])
            finally:
                runtime.stop()

    def test_manual_live_commands_are_idempotent_and_preserve_the_last_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
            )
            with runtime._lock:
                runtime._snapshot["connection"]["status"] = "Stale"
                runtime._snapshot["connection"]["sessionId"] = "42"
                runtime._snapshot["records"] = {"FLIGHT_STATE": [{"entity_id": "1"}]}
            first = runtime.request_live_resync()
            second = runtime.request_live_resync()
            self.assertTrue(first["accepted"])
            self.assertFalse(first["pending"])
            self.assertTrue(second["pending"])
            self.assertEqual("resyncing", runtime.latest()["connection"]["recoveryState"])

            first = runtime.request_live_reconnect()
            second = runtime.request_live_reconnect()
            self.assertFalse(first["pending"])
            self.assertTrue(second["pending"])
            retained = runtime.latest()
            self.assertEqual("Disconnected", retained["connection"]["status"])
            self.assertEqual("0", retained["connection"]["sessionId"])
            self.assertEqual("reconnecting", retained["connection"]["recoveryState"])
            self.assertEqual({"FLIGHT_STATE": [{"entity_id": "1"}]}, retained["records"])

    def test_live_commands_are_rejected_in_replay_or_without_a_session(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            live = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=Path(directory),
            )
            with self.assertRaisesRegex(ValueError, "active session"):
                live.request_live_resync()
            replay = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=Path(directory),
                replay_path=Path(directory) / "capture.ndjson",
            )
            with self.assertRaisesRegex(ValueError, "live mode"):
                replay.request_live_resync()
            with self.assertRaisesRegex(ValueError, "live mode"):
                replay.request_live_reconnect()

    def test_reconnect_keeps_retrying_until_a_late_producer_appears(self) -> None:
        with tempfile.TemporaryDirectory() as directory, socket.socket(
            socket.AF_INET, socket.SOCK_DGRAM
        ) as server, mock.patch.object(fstl, "RELIABLE_WINDOW_US", 120_000):
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=server.getsockname()[1],
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
            )
            runtime.start()
            try:
                _, first_address = server.recvfrom(1200)
                late_hello = None
                late_address = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet_bytes, address = server.recvfrom(1200)
                    if (
                        contract.reference.read_header(packet_bytes)["message_type"] == 2
                        and address != first_address
                    ):
                        late_hello, late_address = packet_bytes, address
                        break
                self.assertIsNotNone(late_hello)
                session = 0xAABBCCDDEEFF0011
                server.sendto(
                    contract.packet(3, contract.welcome_for(late_hello), session_id=session,
                                    sequence=1, sent_us=1_000_000, flags=2),
                    late_address,
                )
                server.sendto(
                    contract.packet(4, contract.session_begin_payload(), session_id=session,
                                    sequence=2, sent_us=1_000_001, flags=2),
                    late_address,
                )
                server.sendto(
                    contract.packet(6, contract.v11_payload("minimal-with-player", ".bin"),
                                    session_id=session, sequence=3, sent_us=1_000_002, flags=2),
                    late_address,
                )
                self.assertTrue(self.wait_for(lambda: runtime.latest()["connection"]["status"] == "Live"))
            finally:
                runtime.stop()


if __name__ == "__main__":
    unittest.main()

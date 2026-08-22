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

    def test_transport_gap_counter_resets_when_session_changes(self) -> None:
        quality = QualityTracker(30, 10, 500)
        payload = contract.heartbeat_request_payload(1, 1_000_000)
        for session_id, sequence in (
            (11, 10),
            (11, 11),
            (22, 0x50000000),
            (22, 0x50000002),
        ):
            quality.observe_packet(contract.packet(
                9, payload, session_id=session_id, sequence=sequence,
                sent_us=1_000_000 + sequence,
            ))
        self.assertEqual(1, quality.packet_gap_count)

    def test_rejected_live_datagram_is_not_persisted_in_capture(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=Path(directory),
            )
            path = runtime.capture.start({"name": "validated-only"})
            runtime.client = mock.Mock()
            runtime.client.receive.side_effect = ValueError("datagram CRC")

            with self.assertRaisesRegex(ValueError, "datagram CRC"):
                runtime._receive_live_datagram(
                    b"invalid", 10, "2026-08-09T10:00:00.000010Z"
                )

            runtime.capture.stop()
            self.assertEqual([], load_capture(path)[1])

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

    def test_runtime_switches_live_to_time_based_replay_without_bridge_restart(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 77, 88
            hello = fstl.pack_header(
                message_type=2, flags=0, session_id=0, sequence=1,
                sent_us=t0, message_id=1, payload=fstl.hello_payload(nonce, t0),
            )
            session = 0xAABBCCDD
            writer = CaptureWriter(root)
            path = writer.start({"name": "dynamic replay"})
            writer.packet(contract.packet(3, contract.welcome_for(hello), session_id=session, sequence=1, sent_us=100, flags=2), 100, "1970-01-01T00:00:00.000100Z")
            writer.packet(contract.packet(4, contract.session_begin_payload(), session_id=session, sequence=2, sent_us=101, flags=2), 101, "1970-01-01T00:00:00.000101Z")
            writer.packet(contract.packet(6, contract.cockpit_snapshot_payload(), session_id=session, sequence=3, sent_us=102, flags=6), 102, "1970-01-01T00:00:00.000102Z")
            writer.stop()
            runtime = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=root,
            )
            runtime.start()
            try:
                capture_id = runtime.captures()[0]["id"]
                replay = runtime.load_replay(capture_id)
                self.assertEqual("microseconds", replay["timelineUnit"])
                loaded = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["replay"]["packetCount"] == 3
                        and snapshot["replay"]["durationUs"] == 2
                        else None
                    )
                )
                self.assertIsNotNone(loaded)
                self.assertFalse(loaded["replay"]["playing"])
                runtime.replay_control(playing=True)
                self.assertTrue(self.wait_for(lambda: runtime.latest()["connection"]["status"] == "Live"))
                self.assertEqual("replay", runtime.latest()["mode"])
                self.assertEqual(session, int(runtime.latest()["connection"]["sessionId"]))
                runtime.replay_control(position_us=0, playing=False)
                self.assertTrue(self.wait_for(lambda: runtime.latest()["replay"]["positionUs"] == 0))
            finally:
                runtime.stop()

    def test_native_replay_path_loads_outside_the_configured_library(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            external = root / "external"
            library = root / "library"
            external.mkdir()
            nonce, t0 = 78, 89
            hello = fstl.pack_header(
                message_type=2,
                flags=0,
                session_id=0,
                sequence=1,
                sent_us=t0,
                message_id=1,
                payload=fstl.hello_payload(nonce, t0),
            )
            session = 0xBBCCDDEE
            writer = CaptureWriter(external)
            path = writer.start({"name": "external replay"})
            writer.packet(
                contract.packet(
                    3,
                    contract.welcome_for(hello),
                    session_id=session,
                    sequence=1,
                    sent_us=100,
                    flags=2,
                ),
                100,
                "1970-01-01T00:00:00.000100Z",
            )
            writer.packet(
                contract.packet(
                    4,
                    contract.session_begin_payload(),
                    session_id=session,
                    sequence=2,
                    sent_us=101,
                    flags=2,
                ),
                101,
                "1970-01-01T00:00:00.000101Z",
            )
            writer.packet(
                contract.packet(
                    6,
                    contract.cockpit_snapshot_payload(),
                    session_id=session,
                    sequence=3,
                    sent_us=102,
                    flags=6,
                ),
                102,
                "1970-01-01T00:00:00.000102Z",
            )
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                replay_path=path,
                capture_dir=library,
            )
            runtime.start()
            try:
                loaded = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["replay"]["packetCount"] == 3
                        else None
                    )
                )
                self.assertIsNotNone(loaded)
                self.assertEqual([], loaded["replay"]["ranges"])
                self.assertEqual([], loaded["replay"]["sessionBoundaries"])
                runtime.replay_control(playing=True)
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.latest()["connection"]["status"] == "Live"
                    )
                )
                self.assertTrue(runtime._thread.is_alive())
                self.assertNotIn("error", runtime.latest()["connection"])
            finally:
                runtime.stop()

    def test_active_range_waits_until_its_exact_end_before_pausing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 79, 90
            hello = fstl.pack_header(
                message_type=2,
                flags=0,
                session_id=0,
                sequence=1,
                sent_us=t0,
                message_id=1,
                payload=fstl.hello_payload(nonce, t0),
            )
            session = 0xCCDDEEFF
            writer = CaptureWriter(root)
            writer.start({"name": "range boundary timing"})
            packets = (
                contract.packet(
                    3,
                    contract.welcome_for(hello),
                    session_id=session,
                    sequence=1,
                    sent_us=100,
                    flags=2,
                ),
                contract.packet(
                    4,
                    contract.session_begin_payload(),
                    session_id=session,
                    sequence=2,
                    sent_us=101,
                    flags=2,
                ),
                contract.packet(
                    6,
                    contract.cockpit_snapshot_payload(),
                    session_id=session,
                    sequence=3,
                    sent_us=102,
                    flags=6,
                ),
                b"future-packet-not-reached",
            )
            for datagram, received_us in zip(
                packets,
                (100, 101, 102, 1_000_102),
            ):
                writer.packet(
                    datagram,
                    received_us,
                    f"1970-01-01T00:00:00.{received_us:06d}Z",
                )
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=root,
            )
            capture_id = runtime.captures()[0]["id"]
            marker = runtime.capture_library.create_range(
                capture_id,
                "exact boundary",
                2,
                200_002,
            )
            runtime.start()
            try:
                runtime.load_replay(capture_id)
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["packetCount"] == 4
                        and any(
                            item["id"] == marker["id"]
                            for item in runtime.replay_state["ranges"]
                        )
                    )
                )
                runtime.replay_control(
                    active_range_id=marker["id"],
                    playing=False,
                )
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["positionUs"] == 2
                        and not runtime.replay_state["seeking"]
                    )
                )

                started = time.monotonic()
                runtime.replay_control(playing=True)
                self.assertTrue(runtime.replay_state["playing"])
                self.assertTrue(
                    self.wait_for(
                        lambda: not runtime.replay_state["playing"]
                        and runtime.replay_state["positionUs"] == 200_002,
                        timeout=1.0,
                    )
                )
                self.assertGreaterEqual(time.monotonic() - started, 0.15)
                self.assertEqual(3, runtime.replay_state["position"])
                self.assertTrue(runtime._thread.is_alive())
            finally:
                runtime.stop()

    def test_microsecond_seek_preserves_playhead_and_waits_for_next_packet(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 87, 98
            hello = fstl.pack_header(
                message_type=2,
                flags=0,
                session_id=0,
                sequence=1,
                sent_us=t0,
                message_id=1,
                payload=fstl.hello_payload(nonce, t0),
            )
            session = 0xFFEEDDCC
            writer = CaptureWriter(root)
            writer.start({"name": "seek timing"})
            writer.packet(
                contract.packet(
                    3,
                    contract.welcome_for(hello),
                    session_id=session,
                    sequence=1,
                    sent_us=100,
                    flags=2,
                ),
                100,
                "1970-01-01T00:00:00.000100Z",
            )
            writer.packet(
                contract.packet(
                    4,
                    contract.session_begin_payload(),
                    session_id=session,
                    sequence=2,
                    sent_us=101,
                    flags=2,
                ),
                101,
                "1970-01-01T00:00:00.000101Z",
            )
            writer.packet(
                contract.packet(
                    6,
                    contract.cockpit_snapshot_payload(),
                    session_id=session,
                    sequence=3,
                    sent_us=1_000_100,
                    flags=6,
                ),
                1_000_100,
                "1970-01-01T00:00:01.000100Z",
            )
            writer.stop()
            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=root,
            )
            runtime.start()
            try:
                capture_id = runtime.captures()[0]["id"]
                runtime.load_replay(capture_id)
                self.assertTrue(
                    self.wait_for(lambda: runtime.replay_state["packetCount"] == 3)
                )
                runtime.replay_control(position_us=500_000, playing=False)
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["position"] == 2
                        and runtime.replay_state["positionUs"] == 500_000
                        and not runtime.replay_state["seeking"]
                    )
                )

                runtime.replay_control(playing=True)
                time.sleep(0.1)
                self.assertEqual(2, runtime.replay_state["position"])
                self.assertNotEqual("Live", runtime.latest()["connection"]["status"])
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["position"] == 3
                        and runtime.latest()["connection"]["status"] == "Live",
                        timeout=1.0,
                    )
                )
            finally:
                runtime.stop()

    def test_failed_preview_and_seek_keep_replay_reloadable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 177, 188
            hello = fstl.pack_header(
                message_type=2, flags=0, session_id=0, sequence=1,
                sent_us=t0, message_id=1, payload=fstl.hello_payload(nonce, t0),
            )
            session = 0x1122AABB
            writer = CaptureWriter(root)
            writer.start({"name": "recoverable seek"})
            writer.packet(contract.packet(3, contract.welcome_for(hello), session_id=session, sequence=1, sent_us=100, flags=2), 100, "1970-01-01T00:00:00.000100Z")
            writer.packet(contract.packet(4, contract.session_begin_payload(), session_id=session, sequence=2, sent_us=101, flags=2), 101, "1970-01-01T00:00:00.000101Z")
            writer.packet(contract.packet(6, contract.cockpit_snapshot_payload(), session_id=session, sequence=3, sent_us=102, flags=6), 102, "1970-01-01T00:00:00.000102Z")
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=root,
            )
            runtime.start()
            try:
                capture_id = runtime.captures()[0]["id"]
                runtime.load_replay(capture_id)
                runtime.replay_control(playing=True)
                self.assertTrue(self.wait_for(lambda: runtime.latest()["connection"]["status"] == "Live"))

                with mock.patch.object(
                    runtime,
                    "_reconstruct_replay_client",
                    side_effect=ValueError("seek reconstruction failed"),
                ) as reconstruct:
                    runtime.replay_preview(1)
                    self.assertTrue(self.wait_for(lambda: reconstruct.call_count >= 1))
                    self.assertEqual("seek reconstruction failed", runtime.latest()["connection"].get("error"))
                    self.assertTrue(runtime._thread.is_alive())

                    runtime.replay_control(position_us=1, playing=False)
                    self.assertTrue(self.wait_for(lambda: reconstruct.call_count >= 2))
                    self.assertFalse(runtime.latest()["replay"]["seeking"])
                    self.assertTrue(runtime._thread.is_alive())

                runtime.load_replay(capture_id)
                runtime.replay_control(playing=True)
                recovered = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["connection"]["status"] == "Live"
                        and snapshot["replay"]["packetCount"] == 3
                        else None
                    )
                )
                self.assertIsNotNone(recovered)
                self.assertNotIn("error", recovered["connection"])
            finally:
                runtime.stop()

    def test_replay_drops_oldest_incomplete_fragment_group_instead_of_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 277, 288
            hello = fstl.pack_header(
                message_type=2, flags=0, session_id=0, sequence=1,
                sent_us=t0, message_id=1, payload=fstl.hello_payload(nonce, t0),
            )
            session = 1
            writer = CaptureWriter(root)
            writer.start({"name": "captured fragment loss"})
            packets = [
                contract.packet(3, contract.welcome_for(hello), session_id=session, sequence=1, sent_us=100, flags=2),
                contract.packet(4, contract.session_begin_payload(), session_id=session, sequence=2, sent_us=101, flags=2),
                contract.packet(6, contract.cockpit_snapshot_payload(), session_id=session, sequence=3, sent_us=102, flags=6),
                *(contract.incomplete_fragment(message_id) for message_id in range(10, 14)),
                contract.packet(
                    9,
                    fstl.heartbeat_payload(1, 1, 108, 0, 0),
                    session_id=session,
                    sequence=14,
                    sent_us=108,
                ),
            ]
            for index, datagram in enumerate(packets):
                observed_us = 100 + index
                writer.packet(
                    datagram,
                    observed_us,
                    f"1970-01-01T00:00:00.{observed_us:06d}Z",
                )
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=root,
            )
            runtime.start()
            try:
                capture_id = runtime.captures()[0]["id"]
                runtime.load_replay(capture_id)
                runtime.replay_control(playing=True)
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["position"] == len(packets)
                        and not runtime.replay_state["playing"]
                    )
                )
                self.assertTrue(runtime._thread.is_alive())
                self.assertNotIn("error", runtime.latest()["connection"])

                runtime.replay_control(position_us=len(packets) - 1, playing=False)
                self.assertTrue(
                    self.wait_for(
                        lambda: runtime.replay_state["position"] == len(packets)
                        and not runtime.replay_state["seeking"]
                    )
                )
                self.assertNotIn("error", runtime.latest()["connection"])
            finally:
                runtime.stop()

    def test_replay_control_publishes_pause_and_resume_immediately(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, replay_path=Path(directory) / "unused.fstlcap",
                capture_dir=Path(directory),
            )
            runtime.replay_state.update({
                "packetCount": 10,
                "durationUs": 1_000_000,
                "position": 4,
                "positionUs": 400_000,
                "playing": True,
            })
            runtime._publish_replay_metadata()

            paused = runtime.replay_control(playing=False)
            self.assertFalse(paused["playing"])
            self.assertFalse(runtime.latest()["replay"]["playing"])
            self.assertEqual(400_000, runtime.latest()["replay"]["positionUs"])

            resumed = runtime.replay_control(playing=True)
            self.assertTrue(resumed["playing"])
            self.assertTrue(runtime.latest()["replay"]["playing"])

    def test_replay_crosses_recorded_reconnect_without_session_end(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = CaptureWriter(root)
            writer.start({"name": "two source sessions"})
            sessions = (0x1111222233334444, 0xAAAABBBBCCCCDDDD)
            received_us = 100
            for index, session in enumerate(sessions):
                nonce = 1000 + index
                t0 = 2000 + index
                hello = fstl.pack_header(
                    message_type=2,
                    flags=0,
                    session_id=0,
                    sequence=1,
                    sent_us=t0,
                    message_id=1,
                    payload=fstl.hello_payload(nonce, t0),
                )
                writer.session_boundary(index, session, received_us - 100, "welcome")
                writer.packet(
                    contract.packet(
                        3,
                        contract.welcome_for(hello),
                        session_id=session,
                        sequence=1,
                        sent_us=received_us,
                        flags=2,
                    ),
                    received_us,
                    f"1970-01-01T00:00:00.{received_us:06d}Z",
                )
                writer.packet(
                    contract.packet(
                        4,
                        contract.session_begin_payload(),
                        session_id=session,
                        sequence=2,
                        sent_us=received_us + 1,
                        flags=2,
                    ),
                    received_us + 1,
                    f"1970-01-01T00:00:00.{received_us + 1:06d}Z",
                )
                writer.packet(
                    contract.packet(
                        6,
                        contract.cockpit_snapshot_payload(),
                        session_id=session,
                        sequence=3,
                        sent_us=received_us + 2,
                        flags=6,
                    ),
                    received_us + 2,
                    f"1970-01-01T00:00:00.{received_us + 2:06d}Z",
                )
                received_us += 1_000
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1",
                port=42042,
                flight_hz=30,
                systems_hz=10,
                mission_heartbeat_ms=500,
                capture_dir=root,
            )
            runtime.start()
            try:
                capture_id = runtime.captures()[0]["id"]
                runtime.load_replay(capture_id)
                runtime.replay_control(playing=True)
                second_session = str(sessions[1])
                snapshot = self.wait_for(
                    lambda: (
                        current
                        if (current := runtime.latest())["connection"]["status"] == "Live"
                        and current["connection"]["sessionId"] == second_session
                        else None
                    )
                )
                self.assertIsNotNone(snapshot)
                self.assertNotIn("error", snapshot["connection"])
                self.assertEqual(6, snapshot["replay"]["position"])
            finally:
                runtime.stop()

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
                        contract.cockpit_snapshot_payload(),
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

    def test_stuck_stale_session_rehandshakes_on_same_endpoint_without_erasing_last_state(self) -> None:
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
                recovery_grace_us=150_000,
            )
            runtime.start()
            try:
                hello, first_address = server.recvfrom(1200)
                first_nonce = int.from_bytes(hello[68:76], "little")
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
                        contract.cockpit_snapshot_payload(),
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
                        and address == first_address
                    ):
                        second_hello, second_address = packet_bytes, address
                        break
                self.assertIsNotNone(second_hello)
                self.assertEqual(first_address, second_address)
                self.assertNotEqual(
                    first_nonce, int.from_bytes(second_hello[68:76], "little")
                )
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
                        contract.cockpit_snapshot_payload(),
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

    def test_replay_rejects_legacy_profile_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nonce, t0 = 77, 88
            hello = fstl.pack_header(
                message_type=2, flags=0, session_id=0, sequence=1,
                sent_us=t0, message_id=1, payload=fstl.hello_payload(nonce, t0),
            )
            session = 0xAABBCCDD
            writer = CaptureWriter(root)
            writer.start({"name": "legacy profile replay"})
            writer.packet(contract.packet(3, contract.welcome_for(hello), session_id=session, sequence=1, sent_us=100, flags=2), 100, "1970-01-01T00:00:00.000100Z")
            writer.packet(contract.packet(4, contract.session_begin_payload(), session_id=session, sequence=2, sent_us=101, flags=2), 101, "1970-01-01T00:00:00.000101Z")
            writer.packet(contract.packet(6, contract.v11_payload("minimal-with-player", ".bin"), session_id=session, sequence=3, sent_us=102, flags=6), 102, "1970-01-01T00:00:00.000102Z")
            writer.stop()

            runtime = TelemetryRuntime(
                host="127.0.0.1", port=42042, flight_hz=30, systems_hz=10,
                mission_heartbeat_ms=500, capture_dir=root,
            )
            runtime.start()
            try:
                runtime.load_replay(runtime.captures()[0]["id"])
                runtime.replay_control(playing=True)
                rejected = self.wait_for(
                    lambda: runtime.latest()["connection"].get("error")
                )
                self.assertIn("CockpitSensors 0x07CB required", rejected)
                self.assertNotEqual("Live", runtime.latest()["connection"]["status"])
            finally:
                runtime.stop()

    def test_replacement_session_is_not_rotated_before_its_snapshot_window(self) -> None:
        with tempfile.TemporaryDirectory() as directory, socket.socket(
            socket.AF_INET, socket.SOCK_DGRAM
        ) as server, mock.patch.object(fstl, "RELIABLE_WINDOW_US", 600_000):
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
                recovery_grace_us=150_000,
            )
            runtime.start()
            try:
                first_hello, address = server.recvfrom(1200)
                first_session = 0x1122334455667788
                for message_type, payload, sequence in (
                    (3, contract.welcome_for(first_hello), 1),
                    (4, contract.session_begin_payload(), 2),
                    (6, contract.cockpit_snapshot_payload(), 3),
                ):
                    server.sendto(
                        contract.packet(message_type, payload, session_id=first_session,
                                        sequence=sequence, sent_us=1_000_000 + sequence, flags=2),
                        address,
                    )
                self.assertTrue(self.wait_for(
                    lambda: runtime.latest()["connection"]["status"] == "Live"
                ))
                self.assertTrue(self.wait_for(
                    lambda: runtime.latest()["connection"]["status"] == "Stale"
                ))

                replacement_hello = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet, packet_address = server.recvfrom(1200)
                    if contract.reference.read_header(packet)["message_type"] == 2:
                        replacement_hello = packet
                        self.assertEqual(address, packet_address)
                        break
                self.assertIsNotNone(replacement_hello)
                replacement_session = 0x8877665544332211
                for message_type, payload, sequence in (
                    (3, contract.welcome_for(replacement_hello), 20),
                    (4, contract.session_begin_payload(), 21),
                ):
                    server.sendto(
                        contract.packet(message_type, payload, session_id=replacement_session,
                                        sequence=sequence, sent_us=2_000_000 + sequence, flags=2),
                        address,
                    )

                server.settimeout(0.35)
                third_hello = None
                try:
                    while True:
                        packet, _ = server.recvfrom(1200)
                        if contract.reference.read_header(packet)["message_type"] == 2:
                            third_hello = packet
                            break
                except socket.timeout:
                    pass
                self.assertIsNone(third_hello)

                server.sendto(
                    contract.packet(
                        6, contract.cockpit_snapshot_payload(),
                        session_id=replacement_session, sequence=22,
                        sent_us=2_000_022, flags=2,
                    ),
                    address,
                )
                recovered = self.wait_for(
                    lambda: (
                        snapshot
                        if (snapshot := runtime.latest())["connection"]["status"] == "Live"
                        and snapshot["connection"]["sessionId"] == str(replacement_session)
                        else None
                    )
                )
                self.assertIsNotNone(recovered)
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

    def test_reconnect_keeps_one_hello_generation_until_a_late_producer_appears(self) -> None:
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
                first_hello, first_address = server.recvfrom(1200)
                first_nonce = int.from_bytes(first_hello[68:76], "little")
                first_t0 = int.from_bytes(first_hello[76:84], "little")
                first_header = contract.reference.read_header(first_hello)
                late_hello = None
                late_address = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet_bytes, address = server.recvfrom(1200)
                    if (
                        contract.reference.read_header(packet_bytes)["message_type"] == 2
                        and address == first_address
                    ):
                        late_hello, late_address = packet_bytes, address
                        break
                self.assertIsNotNone(late_hello)
                self.assertEqual(first_address, late_address)
                self.assertEqual(
                    first_nonce, int.from_bytes(late_hello[68:76], "little")
                )
                self.assertEqual(first_t0, int.from_bytes(late_hello[76:84], "little"))
                late_header = contract.reference.read_header(late_hello)
                self.assertEqual(first_header["message_id"], late_header["message_id"])
                self.assertNotEqual(0, late_header["flags"] & fstl.RETRANSMISSION)
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
                    contract.packet(6, contract.cockpit_snapshot_payload(),
                                    session_id=session, sequence=3, sent_us=1_000_002, flags=2),
                    late_address,
                )
                self.assertTrue(self.wait_for(lambda: runtime.latest()["connection"]["status"] == "Live"))
            finally:
                runtime.stop()


if __name__ == "__main__":
    unittest.main()

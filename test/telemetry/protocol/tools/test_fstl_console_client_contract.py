#!/usr/bin/env python3
"""Short contract tests for the independent FSTL observation client.

The behavioural UDP transcript remains intentionally black-box: the Phase 1
contract defines its observable behaviour but not a Python API.  This guard
keeps the tool location and the D1-013 dependency boundary explicit before
the integration fixture supplies packets to it.
"""

from __future__ import annotations

import ast
import importlib.util
import json
import math
import hashlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parent
CONSOLE = TOOLS / "fstl_console_client.py"
SCENARIO = TOOLS / "fstl_phase2_scenario_client.py"
REPO = TOOLS.parents[3]
V11 = REPO / "test" / "telemetry" / "protocol" / "vectors-v1.1"
OBSERVATIONS = REPO / "test" / "telemetry" / "producer" / "observations"

sys.path.insert(0, str(TOOLS))
import fstl_reference_decoder as reference
import fstl_console_client as console
import fstl_phase2_scenario_client as scenario


def packet(message_type: int, payload: bytes, *, session_id: int, sequence: int, sent_us: int,
           flags: int = 0, message_id: int | None = None) -> bytes:
    """Build a test datagram without importing the console implementation."""
    message_crc = reference.crc32_iso_hdlc(payload)
    prefix = struct.pack(
        "<IBBBBHHQIIqQIHHIII",
        0x4C545346, 1, 1, message_type, flags, 68, len(payload), session_id,
        sequence, 0, 0, sent_us, sequence if message_id is None else message_id, 0, 1, len(payload), 0, message_crc,
    )
    crc = reference.crc32_iso_hdlc(prefix + bytes(4) + payload)
    return prefix + struct.pack("<I", crc) + payload


def incomplete_fragment(message_id: int) -> bytes:
    """Build fragment zero of a valid two-fragment logical message."""
    payload = bytes(reference.MAX_FRAGMENT_PAYLOAD)
    message_size = reference.MAX_FRAGMENT_PAYLOAD + 1
    prefix = struct.pack(
        "<IBBBBHHQIIqQIHHIII",
        0x4C545346, 1, 1, 7, 0, 68, len(payload), 1,
        message_id, 0, 0, message_id, message_id, 0, 2,
        message_size, 0, 0,
    )
    crc = reference.crc32_iso_hdlc(prefix + bytes(4) + payload)
    return prefix + struct.pack("<I", crc) + payload


def v11_payload(name: str, suffix: str) -> bytes:
    return (V11 / name / f"{name}{suffix}").read_bytes()


def snapshot_parts(snapshot_id: int = 1) -> tuple[bytes, bytes]:
    """Split the Phase-1 snapshot's two record regions without console code."""
    payload = v11_payload("minimal-with-player", ".bin")
    region = payload[60:]
    first_length = 6 + int.from_bytes(region[4:6], "little")
    first_length += 6 + int.from_bytes(region[first_length + 4:first_length + 6], "little")
    regions = (region[:first_length], region[first_length:])
    digest = hashlib.sha256(region).digest()
    result: list[bytes] = []
    for index, records in enumerate(regions):
        prefix = struct.pack("<IHHI32sQIHH", snapshot_id, index, 2, len(region), digest,
                             1_000_000, 0, 1, 2 if index == 0 else 2)
        result.append(prefix + records)
    return tuple(result)  # type: ignore[return-value]


def welcome_for(hello: bytes) -> bytes:
    decoded = reference.decode_message(2, reference.read_header(hello)["flags"], hello[68:], {})
    fields = decoded["fields"]
    payload = bytearray(v11_payload("welcome-accepted-minor-one", ".payload.bin"))
    struct.pack_into("<Q", payload, 0, int(fields["client_nonce"]))
    struct.pack_into("<Q", payload, 8, int(fields["client_send_t0_us"]))
    return bytes(payload)


def session_begin_payload() -> bytes:
    payload = bytearray((REPO / "test/telemetry/protocol/vectors/valid/messages/session_begin/session_begin.bin").read_bytes())
    struct.pack_into("<I", payload, 24, 0)  # Phase-1 snapshot has no required manifest.
    return bytes(payload)


def session_end_payload() -> bytes:
    return (REPO / "test/telemetry/protocol/vectors/valid/messages/session_end/session_end.bin").read_bytes()


def phase2_manifest_payload(manifest_id: int) -> bytes:
    payload = bytearray(
        (REPO / "test/telemetry/protocol/vectors/valid/messages/manifest/manifest.bin").read_bytes()
    )
    struct.pack_into("<I", payload, 0, manifest_id)
    struct.pack_into("<I", payload, 62, manifest_id)
    payload[12:44] = hashlib.sha256(payload[56:]).digest()
    return bytes(payload)


def phase2_snapshot_payload(snapshot_id: int, manifest_id: int) -> bytes:
    payload = bytearray(v11_payload("phase2-promotion", ".bin"))
    struct.pack_into("<I", payload, 0, snapshot_id)
    struct.pack_into("<I", payload, 52, manifest_id)
    return bytes(payload)


def delta_payload(name: str, baseline_snapshot_id: int) -> bytes:
    payload = bytearray(v11_payload(name, ".payload.bin"))
    struct.pack_into("<I", payload, 0, baseline_snapshot_id)
    return bytes(payload)


def with_snapshot_transaction_size(payload: bytes, size: int, snapshot_id: int | None = None) -> bytes:
    updated = bytearray(payload)
    if snapshot_id is not None:
        struct.pack_into("<I", updated, 0, snapshot_id)
    struct.pack_into("<I", updated, 8, size)
    return bytes(updated)


def ack_for(header: dict[str, int], flags: int = 1) -> bytes:
    return struct.pack("<IBBHI", header["message_id"], header["message_type"], flags,
                       header["fragment_count"], header["message_crc32"])


def heartbeat_request_payload(probe_id: int, origin_t0_us: int) -> bytes:
    return struct.pack("<IB3xQQQ", probe_id, 1, origin_t0_us, 0, 0)


def receive_message_type(server: socket.socket, message_type: int) -> tuple[bytes, tuple[str, int]]:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        data, client = server.recvfrom(1200)
        if reference.read_header(data)["message_type"] == message_type:
            return data, client
    raise AssertionError(f"did not receive FSTL message type {message_type}")


class FstlConsoleClientContractTest(unittest.TestCase):
    def test_reference_decoder_accepts_optional_groups_for_phase2_ship_records(self) -> None:
        def text(value: str) -> bytes:
            encoded = value.encode("utf-8")
            return struct.pack("<H", len(encoded)) + encoded

        def record(record_type: int, payload: bytes) -> dict[str, object]:
            encoded = struct.pack("<HBBH", record_type, 1, 0, len(payload)) + payload
            return reference.decode_record(encoded)["fields"]

        control = (
            struct.pack("<QQQ6fBI", 1, 0x07, 100, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0, 0x3F)
            + struct.pack("<fHHH4f", 50.0, 1, 2, 3, 0.1, -0.1, 0.5, 0.2)
        )
        self.assertEqual("7", record(8, control)["presence"])

        damage = (
            struct.pack("<QQQffH", 1, 0x3F, 100, 90.0, 100.0, 0x07)
            + struct.pack("<fIffQI", 95.0, 2, 10.0, 25.0, 0, 0)
            + struct.pack("<HBH", 1, 1, 20)
            + struct.pack("<QII f", 0, 0, 0, 25.0)
        )
        self.assertEqual(1, len(record(9, damage)["contributors"]))

        shield = (
            struct.pack("<QQQBHH", 1, 0x07, 100, 1, 1, 0)
            + struct.pack("<5f", 50.0, 100.0, 100.0, 5.0, -2.0)
        )
        self.assertEqual(5.0, record(10, shield)["regeneration_per_s"])

        animation_payload = struct.pack("<HHBBfffQ", 0x0005, 1, 1, 0, 0.1, 0.2, 0.3, 10)
        animation = struct.pack("<BH", 1, len(animation_payload)) + animation_payload
        turret_bank_payload = struct.pack("<HBBHIII fQ", 1, 0, 0, 0, 5, 6, 7, 8.0, 9)
        turret_bank = struct.pack("<BH", 1, len(turret_bank_payload)) + turret_bank_payload
        subsystem = (
            struct.pack("<QIQ QHBffI", 1, 2, 0xFF, 100, 0, 2, 50.0, 100.0, 1)
            + text("turret")
            + text("")
            + text("")
            + struct.pack("<IQ3f4fH", 3, 10, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1)
            + animation
            + struct.pack("<B", 1)
            + text("Cargo")
            + struct.pack("<ff", 50.0, 100.0)
            + struct.pack("<IQI", 0x1FFF, 9, 2)
            + struct.pack("<3f", 1.0, 0.0, 0.0)
            + struct.pack("<3f3f", 1.0, 2.0, 3.0, 0.0, 0.0, 0.0)
            + struct.pack("<HHQQf", 1, 0, 10, 20, 100.0)
            + struct.pack("<hHff", 2, 0, 0.1, 1.0)
            + struct.pack("<BQH", 1, 30, 1)
            + turret_bank
            + struct.pack("<HIff", 2, 5, 1.0, 2.0)
        )
        decoded_subsystem = record(11, subsystem)
        self.assertEqual(1, len(decoded_subsystem["animations"]))
        self.assertEqual(1, len(decoded_subsystem["turret"]["banks"]))

        energy = (
            struct.pack("<QQQ5B", 1, 0x3F, 100, 1, 4, 4, 4, 0)
            + struct.pack("<11f", 50.0, 100.0, 5.0, 6.0, -1.0, 1.0,
                          2.0, 70.0, 3.0, 40.0, 80.0)
        )
        self.assertEqual(50.0, record(12, energy)["weapon_energy_current"])

        propulsion = (
            struct.pack("<QQQHH", 1, 0x3F, 100, 0x0005, 0)
            + struct.pack("<4f", 50.0, 100.0, 5.0, 2.0)
            + struct.pack("<fQQf", 25.0, 10, 20, 70.0)
            + struct.pack("<f3f", 0.7, 0.0, 0.0, 150.0)
            + struct.pack("<f3f", 1.0, 0.0, 0.0, 0.5)
        )
        self.assertEqual(50.0, record(13, propulsion)["fuel_current"])

    def test_reference_decoder_accepts_every_ship_identity_optional_group(self) -> None:
        def text(value: str) -> bytes:
            encoded = value.encode("utf-8")
            return struct.pack("<H", len(encoded)) + encoded

        payload = (
            struct.pack("<QQQI", 7, 0x1F, 1_000_000, 3)
            + text("Alpha 1")
            + text("GTF Ulysses")
            + text("Maverick")
            + struct.pack("<IIIHf", 1, 2, 3, 0x000F, 16.0)
            + struct.pack("<IH", 4, 0)
            + text("Alpha")
            + struct.pack("<fH", 18.0, 0x000F)
        )
        encoded = struct.pack("<HBBH", 6, 1, 0, len(payload)) + payload

        decoded = reference.decode_record(encoded)
        fields = decoded["fields"]

        self.assertEqual("7", fields["entity_id"])
        self.assertEqual("31", fields["presence"])
        self.assertEqual("GTF Ulysses", fields["display_name"])
        self.assertEqual("Maverick", fields["callsign"])
        self.assertEqual(
            {"wing_id": 4, "wing_name": "Alpha", "wing_position": 0},
            fields["wing"],
        )
        self.assertEqual(18.0, fields["logical_size"])
        self.assertEqual(0x000F, fields["sensor_visibility_flags"])

    def test_phase2_scenario_tool_is_external_and_keeps_the_neutral_console_unchanged(self) -> None:
        self.assertTrue(SCENARIO.is_file())
        tree = ast.parse(SCENARIO.read_text(encoding="utf-8"))
        imports = {
            alias.name
            for node in ast.walk(tree)
            if isinstance(node, ast.Import)
            for alias in node.names
        }
        imported_modules = imports | {
            node.module or ""
            for node in ast.walk(tree)
            if isinstance(node, ast.ImportFrom)
        }
        self.assertIn("fstl_console_client", imports)
        self.assertIn("fstl_reference_decoder", imports)
        self.assertFalse(
            any(
                name.startswith(("telemetry", "code", "producer"))
                for name in imported_modules
            )
        )

    def test_phase2_scenario_state_transcript_is_compact_and_keeps_final_human_evidence(self) -> None:
        client = scenario.ScenarioConsoleClient(
            None, 3_000_000, "fast", lambda event: None
        )
        client.state.status = "Live"
        client.state.session_id = 0x1234
        client.state.manifest_id = 7
        client.state.required_manifest_id = 7
        client.state.manifest_applied = True
        client.state.keyframe_applied = True
        client.state.baseline = 9
        client.state.delta_sequence = 11
        client.state.records = {
            "SESSION_STATE": {"observed_player_entity_id": "1"},
            "MISSION_STATE": {"mission_generation": 3},
        }
        client.state.record_instances = {
            "CLASS_MANIFEST/manifest_generation=7/class_id=1": {
                "recordName": "CLASS_MANIFEST",
                "fields": {"manifest_generation": 7, "class_id": 1, "large": "x" * 100_000},
            },
            "SUPPORT_STATE/entity_id=2": {
                "recordName": "SUPPORT_STATE",
                "fields": {"entity_id": "2", "phase": 3},
            },
        }

        event = scenario._compact_state_event(
            client, 1_000_000, "2026-08-02T12:00:00.000000Z"
        )
        encoded = json.dumps(event, separators=(",", ":"))
        self.assertLess(len(encoded), 1024)
        self.assertNotIn("dashboard", event)
        self.assertNotIn("records", event)
        self.assertTrue(event["synchronized"])
        self.assertEqual(3, event["missionGeneration"])
        self.assertEqual("1", event["playerEntityId"])

        facts = client.facts()
        self.assertEqual(
            [{
                "identity": "SUPPORT_STATE/entity_id=2",
                "recordName": "SUPPORT_STATE",
                "fields": {"entity_id": "2", "phase": 3},
            }],
            facts["evidenceRecords"],
        )

    def test_phase2_scenario_drains_every_available_datagram_per_ready_socket(self) -> None:
        class ReadySocket:
            def __init__(self) -> None:
                self.datagrams = [bytes([index]) for index in range(8)]

            def recv(self, maximum: int) -> bytes:
                self.asserted_maximum = maximum
                if not self.datagrams:
                    raise BlockingIOError()
                return self.datagrams.pop(0)

        class RecordingClient:
            def __init__(self) -> None:
                self.received: list[bytes] = []

            def receive(self, data: bytes, at_us: int, at_utc: str) -> bool:
                self.received.append(data)
                return False

        ready = ReadySocket()
        client = RecordingClient()
        self.assertEqual(8, scenario._drain_socket(ready, client))
        self.assertEqual([bytes([index]) for index in range(8)], client.received)
        self.assertEqual(console.MAX_DATAGRAM, ready.asserted_maximum)

    def test_incomplete_reassemblies_expire_before_the_bounded_quota_is_reused(self) -> None:
        client = console.ConsoleClient(None, 1_000_000)
        for message_id in range(1, 5):
            self.assertFalse(
                client.receive(
                    incomplete_fragment(message_id),
                    message_id,
                    "2026-08-02T12:00:00.000000Z",
                )
            )
        self.assertEqual(4, len(client.fragments))

        self.assertFalse(
            client.receive(
                incomplete_fragment(5),
                client.state.reliable_reassembly_timeout_us + 4,
                "2026-08-02T12:00:05.000000Z",
            )
        )
        self.assertEqual([(1, 5)], list(client.fragments))

    def test_two_client_applied_policy_holds_one_new_manifest_snapshot_and_releases_bounded_acks(self) -> None:
        events: list[dict[str, object]] = []
        independent = scenario.AppliedSnapshotHold(10_000, events.append, "independent")
        slow = scenario.AppliedSnapshotHold(10_000, events.append, "slow")

        def header(message_id: int) -> dict[str, int]:
            return {
                "message_type": 6,
                "message_id": message_id,
                "message_crc32": 0x10203040 + message_id,
                "fragment_count": 2,
            }

        self.assertFalse(
            independent.route(header(1), console.ACK_APPLIED, 1, 10, 1_000)
        )
        self.assertFalse(
            slow.route(header(1), console.ACK_APPLIED, 1, 10, 1_000)
        )
        self.assertTrue(
            slow.route(header(2), console.ACK_APPLIED, 2, 11, 2_000)
        )
        self.assertTrue(
            slow.route(header(3), console.ACK_APPLIED, 2, 11, 2_100)
        )
        self.assertEqual(2, len(slow.held))
        self.assertFalse(independent.hold_started)
        self.assertTrue(slow.hold_active)
        with self.assertRaisesRegex(ValueError, "second snapshot transaction"):
            slow.route(header(5), console.ACK_APPLIED, 2, 12, 2_200)

        slow.observe_manifest(3, 2_300)
        sent: list[tuple[int, int]] = []
        self.assertFalse(
            slow.release_due(
                11_999,
                lambda ack_header, flags: sent.append((ack_header["message_id"], flags)),
            )
        )
        self.assertTrue(
            slow.release_due(
                12_000,
                lambda ack_header, flags: sent.append((ack_header["message_id"], flags)),
            )
        )
        self.assertEqual(
            [(2, console.ACK_APPLIED), (3, console.ACK_APPLIED)], sent
        )
        self.assertFalse(slow.hold_active)
        self.assertEqual([3], slow.manifests_during_hold)
        self.assertFalse(
            slow.route(header(4), console.ACK_APPLIED, 3, 12, 13_000),
            "the deterministic policy holds exactly one logical snapshot transaction",
        )
        self.assertEqual(
            1,
            sum(event.get("event") == "snapshot-applied-held" for event in events),
        )
        self.assertEqual(
            1,
            sum(event.get("event") == "snapshot-applied-released" for event in events),
        )

    def test_phase2_scenario_cli_rejects_invalid_bounds_before_opening_sockets(self) -> None:
        for arguments in (
            ("--port", "0"),
            ("--seconds", "0"),
            ("--stale-ms", "0"),
            ("--slow-hold-ms", "0"),
        ):
            result = subprocess.run(
                [sys.executable, "-B", str(SCENARIO), *arguments],
                capture_output=True,
                text=True,
                timeout=3,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("invalid port, duration, stale timeout or slow hold", result.stderr)

    def test_slow_client_sends_validated_holds_applied_and_releases_before_n_plus_two(self) -> None:
        class Sender:
            def __init__(self) -> None:
                self.packets: list[bytes] = []

            def send(self, packet_bytes: bytes) -> None:
                self.packets.append(packet_bytes)

        events: list[dict[str, object]] = []
        sender = Sender()
        client = scenario.ScenarioConsoleClient(
            sender, 3_000_000, "slow", events.append, applied_hold_us=80_000
        )
        client.state.welcomed = True
        client.state.session_begun = True
        client.state.session_id = 0x2200
        client.state.required_manifest_id = 1
        client.state.manifest_applied = False
        observed_at_utc = "2026-08-01T12:00:00.000000Z"
        sequence = 1

        def receive(message_type: int, payload: bytes, at_us: int) -> None:
            nonlocal sequence
            client.receive(
                packet(
                    message_type,
                    payload,
                    session_id=client.state.session_id,
                    sequence=sequence,
                    sent_us=at_us,
                    flags=2,
                ),
                at_us,
                observed_at_utc,
            )
            sequence += 1

        def ack_flags_since(index: int) -> list[int]:
            result: list[int] = []
            for ack_packet in sender.packets[index:]:
                ack_header = reference.read_header(ack_packet)
                if ack_header["message_type"] != 10:
                    continue
                decoded = reference.decode_message(
                    10, ack_header["flags"], ack_packet[68:], {}
                )
                result.append(int(decoded["fields"]["ack_flags"]))
            return result

        receive(5, phase2_manifest_payload(1), 1_000_000)
        receive(6, phase2_snapshot_payload(1, 1), 1_100_000)
        sender.packets.clear()

        receive(5, phase2_manifest_payload(2), 2_000_000)
        sender.packets.clear()
        receive(6, phase2_snapshot_payload(2, 2), 2_100_000)
        self.assertEqual([console.ACK_VALIDATED], ack_flags_since(0))
        self.assertTrue(client.hold and client.hold.hold_active)
        self.assertEqual(
            ["ack-sent", "snapshot-applied-held", "ack-held", "snapshot-committed"],
            [event["event"] for event in events[-4:]],
        )
        self.assertTrue(
            all("observedAtMonotonicUs" in event and "observedAtUtc" in event for event in events[-4:])
        )
        held_ack = next(event for event in events if event["event"] == "ack-held")
        self.assertEqual(client.state.session_id, held_ack["sessionId"])
        self.assertRegex(str(held_ack["targetMessageCrc32"]), r"^0x[0-9a-f]{8}$")

        before_release = len(sender.packets)
        client.poll_scenario(2_179_999)
        self.assertEqual([], ack_flags_since(before_release))
        client.poll_scenario(2_180_000)
        self.assertEqual([console.ACK_APPLIED], ack_flags_since(before_release))
        self.assertFalse(client.hold and client.hold.hold_active)
        released_ack = next(event for event in events if event["event"] == "ack-released")
        self.assertEqual(held_ack["targetMessageId"], released_ack["targetMessageId"])
        self.assertEqual(held_ack["targetMessageCrc32"], released_ack["targetMessageCrc32"])

        sender.packets.clear()
        receive(5, phase2_manifest_payload(3), 2_200_000)
        receive(6, phase2_snapshot_payload(3, 3), 2_300_000)
        self.assertIn(console.ACK_APPLIED, ack_flags_since(0))
        self.assertEqual([1, 2, 3], client.manifest_ids)
        self.assertEqual([1, 2, 3], client.snapshot_ids)
        self.assertEqual([], client.hold.manifests_during_hold if client.hold else None)

    def test_fast_scenario_client_reports_one_delta_drop_then_cumulative_convergence(self) -> None:
        class Sender:
            def __init__(self) -> None:
                self.packets: list[bytes] = []

            def send(self, packet_bytes: bytes) -> None:
                self.packets.append(packet_bytes)

        events: list[dict[str, object]] = []
        client = scenario.ScenarioConsoleClient(
            Sender(),
            3_000_000,
            "fast",
            events.append,
            drop_once_delta=True,
        )
        client.state.welcomed = True
        client.state.session_begun = True
        client.state.session_id = 0x3300
        client.state.required_manifest_id = 1
        observed_at_utc = "2026-08-01T12:00:00.000000Z"

        for sequence, (message_type, payload) in enumerate(
            (
                (5, phase2_manifest_payload(1)),
                (6, phase2_snapshot_payload(1, 1)),
                (7, delta_payload("delta-player-kinematics-cumulative", 1)),
                (7, delta_payload("delta-player-return-baseline", 1)),
            ),
            start=1,
        ):
            client.receive(
                packet(
                    message_type,
                    payload,
                    session_id=client.state.session_id,
                    sequence=sequence,
                    sent_us=1_000_000 + sequence,
                    flags=2 if message_type in (5, 6) else 0,
                ),
                1_000_000 + sequence,
                observed_at_utc,
            )

        self.assertEqual(1, client.delta_dropped_count)
        self.assertEqual(1, client.delta_applied_count)
        self.assertEqual(0, client.delta_rejected_count)
        self.assertEqual(3, client.state.delta_sequence)
        self.assertEqual(
            ["delta-dropped", "delta-applied"],
            [event["event"] for event in events if str(event.get("event", "")).startswith("delta-")],
        )
        delta_events = [
            event for event in events if str(event.get("event", "")).startswith("delta-")
        ]
        self.assertEqual([1, 1], [event["receivedBaselineSnapshotId"] for event in delta_events])
        self.assertEqual([2, 3], [event["receivedDeltaSequence"] for event in delta_events])
        self.assertEqual([0, 3], [event["resultingDeltaSequence"] for event in delta_events])
        self.assertEqual(
            [{"action": "drop-once", "message": "delta", "message_id": 3}],
            client.state.injections,
        )

    def test_phase2_scenario_opens_two_independent_udp_sessions(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(2)
            process = subprocess.Popen(
                [
                    sys.executable,
                    "-B",
                    str(SCENARIO),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(server.getsockname()[1]),
                    "--seconds",
                    "0.4",
                    "--slow-hold-ms",
                    "10",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            peers: dict[tuple[str, int], tuple[bytes, int]] = {}
            while len(peers) < 2:
                datagram, peer = server.recvfrom(1200)
                if reference.read_header(datagram)["message_type"] == 2:
                    peers.setdefault(peer, (datagram, 0x1000 + len(peers)))
            first, second = snapshot_parts()
            for peer, (hello, session_id) in peers.items():
                server.sendto(
                    packet(
                        3,
                        welcome_for(hello),
                        session_id=session_id,
                        sequence=1,
                        sent_us=1_000_000,
                        flags=2,
                    ),
                    peer,
                )
                server.sendto(
                    packet(
                        4,
                        session_begin_payload(),
                        session_id=session_id,
                        sequence=2,
                        sent_us=1_000_001,
                        flags=2,
                    ),
                    peer,
                )
                server.sendto(
                    packet(
                        6,
                        first,
                        session_id=session_id,
                        sequence=3,
                        sent_us=1_000_002,
                        flags=2,
                    ),
                    peer,
                )
                server.sendto(
                    packet(
                        6,
                        second,
                        session_id=session_id,
                        sequence=4,
                        sent_us=1_000_003,
                        flags=2,
                    ),
                    peer,
                )
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)
        lines = [json.loads(line) for line in stdout.splitlines()]
        summary = next(line for line in lines if line["kind"] == "scenario-summary")
        self.assertNotEqual(summary["fast"]["sessionId"], summary["slow"]["sessionId"])
        self.assertEqual("Live", summary["fast"]["finalStatus"])
        self.assertEqual("Live", summary["slow"]["finalStatus"])

    def test_complete_ship_fixture_is_neutral_and_spawns_external_cargo(self) -> None:
        mission_path = OBSERVATIONS / "telemetry_p2_complete.fs2"
        mission = mission_path.read_text(encoding="utf-8")
        config = json.loads(
            (OBSERVATIONS / "complete-ship.telemetry.json").read_text(encoding="utf-8")
        )
        self.assertLess(len(mission_path.stem), 28)
        self.assertEqual(1, config["maxClients"])
        cargo = mission.split("$Name: External Cargo 1", 1)[1].split("#Wings", 1)[0]
        self.assertIn("$Class: TC 2", cargo)
        self.assertNotIn("$Class: Cargo Container", cargo)
        self.assertIn("$Arrival Cue: ( true )", cargo)
        self.assertNotIn("( change-ship-class ", mission)

    def test_core_gate_fixture_is_listable_and_spawns_external_cargo(self) -> None:
        mission_path = OBSERVATIONS / "telemetry_p2_core.fs2"
        mission = mission_path.read_text(encoding="utf-8")
        self.assertLess(len(mission_path.stem), 28)
        cargo = mission.split("$Name: External Cargo 1", 1)[1].split("#Wings", 1)[0]
        self.assertIn("$Class: TC 2", cargo)
        self.assertNotIn("$Class: Cargo Container", cargo)
        self.assertIn("$Arrival Cue: ( true )", cargo)

    def test_phase2_dashboard_formulas_are_explicit_and_fail_closed(self) -> None:
        decoded = reference.decode_message(
            6, 0, v11_payload("phase2-complete-ship", ".bin"), {}
        )
        state = console.ConsoleState()
        state.status = "Live"
        state.session_id = 0x1122334455667788
        state.manifest_id = 1
        state.required_manifest_id = 1
        state.baseline = 1
        state._apply_records(decoded["fields"]["records"], True)

        dashboard = console.DashboardProjection(
            state, at_us=1_100_000, smoothed_offset_us=0
        ).build()
        inventory = {item["path"]: item for item in dashboard["inventory"]}
        self.assertEqual(0.5, inventory["entities.1.hull_ratio"]["value"])
        self.assertFalse(inventory["entities.1.shield_ratio"]["available"])
        self.assertEqual("shields-absent", inventory["entities.1.shield_ratio"]["reason"])
        self.assertFalse(inventory["entities.1.subsystems.2.integrity_ratio"]["available"])
        self.assertEqual(
            "missing-or-nonpositive-denominator",
            inventory["entities.1.subsystems.2.integrity_ratio"]["reason"],
        )
        self.assertEqual(0.0, inventory["entities.1.speed"]["value"])
        self.assertEqual([0.0, 0.0, 0.0], inventory["entities.1.velocity_local"]["value"])
        self.assertEqual("ACTIVE", inventory["entities.1.lifecycle_label"]["value"])
        self.assertTrue(inventory["records.FLIGHT_STATE/entity_id=1.age_us"]["available"])
        self.assertTrue(all(
            item.get("formula", "").strip()
            for item in dashboard["inventory"] if item["kind"] == "D"
        ), "P2-TST-048: every derived field has an explicit tested formula")
        self.assertTrue(all(
            item.get("source", "").strip()
            for item in dashboard["inventory"] if item["kind"] in ("A", "C")
        ), "every raw field has non-empty wire provenance")

        without_clock = console.DashboardProjection(state, at_us=1_100_000).build()
        age = next(item for item in without_clock["inventory"]
                   if item["path"] == "records.FLIGHT_STATE/entity_id=1.age_us")
        self.assertFalse(age["available"])
        self.assertEqual("invalid-or-missing-clock-offset", age["reason"])

    def test_integrity_dashboard_derivations_cover_damage_shields_and_subsystems(self) -> None:
        def record(name: str, fields: dict[str, object]) -> dict[str, object]:
            return {
                "recordName": name,
                "recordVersion": 1,
                "fields": fields,
            }

        state = console.ConsoleState()
        state.record_instances = {
            "DAMAGE_STATE/entity_id=1": record(
                "DAMAGE_STATE",
                {
                    "entity_id": "1",
                    "hull_strength": 25.0,
                    "dynamic_max_hull": 100.0,
                    "guardian_threshold": 10.0,
                },
            ),
            "SHIELD_STATE/entity_id=1": record(
                "SHIELD_STATE",
                {
                    "entity_id": "1",
                    "has_shields": 1,
                    "segment_current_hits": [0.0, 25.0, 100.0],
                    "segment_max_hits": [100.0, 50.0, 100.0],
                    "regeneration_per_s": 25.0,
                },
            ),
            "SUBSYSTEM_STATE/entity_id=1/subsystem_id=2": record(
                "SUBSYSTEM_STATE",
                {
                    "entity_id": "1",
                    "subsystem_id": 2,
                    "current_hits": 0.0,
                    "max_hits": 100.0,
                },
            ),
            "SUBSYSTEM_STATE/entity_id=1/subsystem_id=3": record(
                "SUBSYSTEM_STATE",
                {
                    "entity_id": "1",
                    "subsystem_id": 3,
                    "current_hits": 0.0,
                    "max_hits": 0.0,
                },
            ),
        }

        def derived() -> dict[str, dict[str, object]]:
            return console.DashboardProjection(
                state, at_us=1_000_000
            ).build()["derived"]

        values = derived()
        self.assertEqual(0.25, values["entities.1.hull_ratio"]["value"])
        self.assertEqual(75.0, values["entities.1.hull_missing_hits"]["value"])
        self.assertEqual(0.75, values["entities.1.hull_damage_ratio"]["value"])
        self.assertEqual(15.0, values["entities.1.guardian_margin"]["value"])
        self.assertEqual(125.0, values["entities.1.shield_current_total"]["value"])
        self.assertEqual(250.0, values["entities.1.shield_max_total"]["value"])
        self.assertEqual(0.5, values["entities.1.shield_ratio"]["value"])
        self.assertEqual(
            [0.0, 0.5, 1.0],
            values["entities.1.shield_segment_ratios"]["value"],
        )
        self.assertEqual(0, values["entities.1.shield_weakest_segment_index"]["value"])
        self.assertEqual(0.0, values["entities.1.shield_weakest_segment_ratio"]["value"])
        self.assertEqual(125.0, values["entities.1.shield_deficit"]["value"])
        self.assertEqual(5.0, values["entities.1.shield_recharge_eta_s"]["value"])
        self.assertTrue(values["entities.1.subsystems.2.destroyed"]["value"])
        self.assertEqual(100.0, values["entities.1.subsystems.2.missing_hits"]["value"])
        self.assertFalse(values["entities.1.subsystems.3.destroyed"]["value"])
        self.assertFalse(values["entities.1.subsystems.3.integrity_ratio"]["available"])
        self.assertEqual(0.0, values["entities.1.subsystems.3.missing_hits"]["value"])

        damage = state.record_instances["DAMAGE_STATE/entity_id=1"]["fields"]
        damage["hull_strength"] = 0.0
        damage.pop("guardian_threshold")
        zero_hull = derived()
        self.assertEqual(0.0, zero_hull["entities.1.hull_ratio"]["value"])
        self.assertEqual(1.0, zero_hull["entities.1.hull_damage_ratio"]["value"])
        self.assertFalse(zero_hull["entities.1.guardian_margin"]["available"])
        self.assertEqual(
            "guardian-absent",
            zero_hull["entities.1.guardian_margin"]["reason"],
        )

        shield = state.record_instances["SHIELD_STATE/entity_id=1"]["fields"]
        shield["segment_current_hits"] = [100.0, 50.0, 100.0]
        shield["regeneration_per_s"] = 0.0
        full = derived()
        self.assertEqual(0.0, full["entities.1.shield_recharge_eta_s"]["value"])

        shield["segment_current_hits"] = [0.0, 0.0, 0.0]
        blocked = derived()
        self.assertFalse(blocked["entities.1.shield_recharge_eta_s"]["available"])
        self.assertEqual(
            "shield-regeneration-unavailable",
            blocked["entities.1.shield_recharge_eta_s"]["reason"],
        )

        shield.update({
            "has_shields": 0,
            "segment_current_hits": [],
            "segment_max_hits": [],
        })
        absent = derived()
        for suffix in (
            "shield_current_total",
            "shield_max_total",
            "shield_ratio",
            "shield_segment_ratios",
            "shield_deficit",
            "shield_recharge_eta_s",
        ):
            self.assertFalse(absent[f"entities.1.{suffix}"]["available"])
            self.assertEqual("shields-absent", absent[f"entities.1.{suffix}"]["reason"])

    def test_energy_dashboard_derivations_use_propulsion_and_fail_closed(self) -> None:
        def record(name: str, fields: dict[str, object]) -> dict[str, object]:
            return {
                "recordName": name,
                "recordVersion": 1,
                "fields": fields,
            }

        state = console.ConsoleState()
        state.record_instances = {
            "SHIP_IDENTITY/entity_id=1": record(
                "SHIP_IDENTITY", {"entity_id": "1", "ship_class_id": 7}
            ),
            "ENERGY_STATE/entity_id=1": record(
                "ENERGY_STATE",
                {
                    "entity_id": "1",
                    "weapon_energy_current": 40.0,
                    "weapon_energy_max": 80.0,
                    "aggregate_engine_current_hits": 30.0,
                    "aggregate_engine_max_hits": 60.0,
                },
            ),
            "PROPULSION_STATE/entity_id=1": record(
                "PROPULSION_STATE",
                {
                    "entity_id": "1",
                    "propulsion_flags": 0x0001,
                    "fuel_current": 40.0,
                    "fuel_max": 100.0,
                    "consumption_per_s": 10.0,
                    "recovery_per_s": 5.0,
                    "cooldown_remaining_us": "2000000",
                },
            ),
        }
        state.manifest_records = {
            "CLASS_MANIFEST/class_id=7": record(
                "CLASS_MANIFEST",
                {
                    "class_id": 7,
                    "afterburner": {
                        "minimum_start_fuel": 50.0,
                        "burn_rate": 10.0,
                        "recover_rate": 5.0,
                    },
                },
            )
        }

        def values() -> dict[str, dict[str, object]]:
            projection = console.DashboardProjection(state, at_us=1_000_000).build()
            return {
                path: projection["derived"][f"entities.1.{path}"]
                for path in (
                    "weapon_energy_ratio",
                    "engine_integrity_ratio",
                    "fuel_ratio",
                    "afterburner_autonomy_s",
                    "afterburner_recharge_s",
                    "afterburner_usable_fuel",
                    "afterburner_ready_delay_s",
                    "afterburner_readiness",
                )
            }

        result = values()
        self.assertEqual(0.5, result["weapon_energy_ratio"]["value"])
        self.assertEqual(0.5, result["engine_integrity_ratio"]["value"])
        self.assertEqual(0.4, result["fuel_ratio"]["value"])
        self.assertEqual(4.0, result["afterburner_autonomy_s"]["value"])
        self.assertEqual(12.0, result["afterburner_recharge_s"]["value"])
        self.assertEqual(0.0, result["afterburner_usable_fuel"]["value"])
        self.assertEqual(2.0, result["afterburner_ready_delay_s"]["value"])
        self.assertEqual("ATTENTE", result["afterburner_readiness"]["value"])

        state.record_instances["PROPULSION_STATE/entity_id=1"]["fields"]["propulsion_flags"] = 0x0005
        active = values()
        self.assertEqual("ACTIF", active["afterburner_readiness"]["value"])
        self.assertFalse(active["afterburner_recharge_s"]["available"])
        self.assertEqual("afterburner-active", active["afterburner_recharge_s"]["reason"])

        state.record_instances["PROPULSION_STATE/entity_id=1"]["fields"]["propulsion_flags"] = 0x0003
        locked = values()
        self.assertEqual("VERROUILLÉ", locked["afterburner_readiness"]["value"])
        self.assertFalse(locked["afterburner_ready_delay_s"]["available"])
        self.assertEqual("afterburner-locked", locked["afterburner_ready_delay_s"]["reason"])

        propulsion = state.record_instances["PROPULSION_STATE/entity_id=1"]["fields"]
        propulsion["propulsion_flags"] = 0x0001
        propulsion["fuel_current"] = 0.0
        propulsion["recovery_per_s"] = 0.0
        blocked = values()
        self.assertFalse(blocked["afterburner_readiness"]["available"])
        self.assertEqual(
            "below-minimum-without-recovery",
            blocked["afterburner_readiness"]["reason"],
        )
        self.assertEqual(0.0, blocked["fuel_ratio"]["value"])

        propulsion["propulsion_flags"] = 0
        unavailable = values()
        self.assertFalse(unavailable["afterburner_readiness"]["available"])
        self.assertEqual("afterburner-unavailable", unavailable["afterburner_readiness"]["reason"])

    def test_weapon_dashboard_derivations_use_decoded_shapes_and_fail_closed(self) -> None:
        def record(
            name: str,
            fields: dict[str, object],
            version: int = 1,
        ) -> dict[str, object]:
            return {
                "recordName": name,
                "recordVersion": version,
                "fields": fields,
            }

        state = console.ConsoleState()
        state.record_instances = {
            "WEAPON_STATE/entity_id=1": record(
                "WEAPON_STATE",
                {
                    "entity_id": "1",
                    "weapon_flags": 0,
                    "primary_banks": [
                        {
                            "bank_id": 11,
                            "weapon_class_id": 101,
                            "cooldown_remaining_us": "500000",
                            "ammunition": {
                                "current": 20,
                                "initial": 40,
                                "rearm_remaining_us": "2000000",
                            },
                        },
                        {
                            "bank_id": 12,
                            "weapon_class_id": 102,
                            "cooldown_remaining_us": "0",
                        },
                    ],
                    "secondary_banks": [
                        {
                            "bank_id": 21,
                            "weapon_class_id": 201,
                            "cooldown_remaining_us": "0",
                            "ammunition": {"current": 0, "initial": 8},
                        }
                    ],
                    "tertiary": {
                        "bank_id": 31,
                        "ammunition_current": 3,
                        "ammunition_initial": 6,
                        "cooldown_remaining_us": "1000000",
                        "rearm_remaining_us": "3000000",
                    },
                    "countermeasure": {
                        "flags": 0,
                        "current": 2,
                        "maximum": 4,
                        "cooldown_remaining_us": "250000",
                    },
                },
            )
        }
        state.manifest_records = {
            "WEAPON_MANIFEST/weapon_class_id=101": record(
                "WEAPON_MANIFEST",
                {"weapon_class_id": 101, "fire": {"wait_us": "250000"}},
            ),
            "WEAPON_MANIFEST/weapon_class_id=102": record(
                "WEAPON_MANIFEST",
                {"weapon_class_id": 102},
            ),
        }

        def projection() -> dict[str, dict[str, object]]:
            return console.DashboardProjection(
                state, at_us=1_000_000
            ).build()["derived"]

        values = projection()
        self.assertEqual(0.5, values["entities.1.primary_banks[0].ammo_ratio"]["value"])
        self.assertEqual(0.5, values["entities.1.primary_banks[0].cooldown_s"]["value"])
        self.assertEqual(2.0, values["entities.1.primary_banks[0].rearm_s"]["value"])
        self.assertEqual("RECHARGE", values["entities.1.primary_banks[0].state"]["value"])
        self.assertFalse(values["entities.1.primary_banks[1].ammo_ratio"]["available"])
        self.assertEqual(
            "bank-does-not-use-ammunition",
            values["entities.1.primary_banks[1].ammo_ratio"]["reason"],
        )
        self.assertEqual("DISPONIBLE", values["entities.1.primary_banks[1].state"]["value"])
        self.assertEqual(0.0, values["entities.1.secondary_banks[0].ammo_ratio"]["value"])
        self.assertEqual("VIDE", values["entities.1.secondary_banks[0].state"]["value"])
        self.assertEqual(0.5, values["entities.1.tertiary.ammo_ratio"]["value"])
        self.assertEqual(1.0, values["entities.1.tertiary.cooldown_s"]["value"])
        self.assertEqual(3.0, values["entities.1.tertiary.rearm_s"]["value"])
        self.assertEqual("RECHARGE", values["entities.1.tertiary.state"]["value"])
        self.assertEqual(
            0.5, values["entities.1.countermeasure.quantity_ratio"]["value"]
        )
        self.assertEqual(
            "RECHARGE", values["entities.1.countermeasure.state"]["value"]
        )
        self.assertEqual(
            0.25, values["entities.1.countermeasure.cooldown_s"]["value"]
        )
        self.assertEqual(4.0, values["weapon_classes.101.nominal_rate_hz"]["value"])
        self.assertFalse(values["weapon_classes.102.nominal_rate_hz"]["available"])

        weapon = state.record_instances["WEAPON_STATE/entity_id=1"]["fields"]
        weapon["weapon_flags"] = 0x0010
        weapon["countermeasure"]["flags"] = 0x0002
        locked = projection()
        self.assertEqual(
            "VERROUILLÃ‰E", locked["entities.1.primary_banks[0].state"]["value"]
        )
        self.assertEqual(
            "VERROUILLÃ‰E", locked["entities.1.countermeasure.state"]["value"]
        )

    def test_support_and_cargo_dashboard_derivations_are_geometric_not_predictive(self) -> None:
        def record(name: str, fields: dict[str, object]) -> dict[str, object]:
            return {"recordName": name, "recordVersion": 1, "fields": fields}

        state = console.ConsoleState()
        state.record_instances = {
            "CARGO_SCAN_STATE/entity_id=1": record(
                "CARGO_SCAN_STATE",
                {
                    "entity_id": "1",
                    "scan_phase": 2,
                    "elapsed_us": "2500000",
                    "required_us": "10000000",
                },
            ),
            "SUPPORT_STATE/entity_id=1": record(
                "SUPPORT_STATE",
                {"entity_id": "1", "phase": 2, "support_entity_id": "2"},
            ),
            "FLIGHT_STATE/entity_id=1": record(
                "FLIGHT_STATE",
                {
                    "entity_id": "1",
                    "position_world": [0.0, 0.0, 0.0],
                    "velocity_world": [0.0, 0.0, 0.0],
                    "orientation_local_to_world": [1.0, 0.0, 0.0, 0.0],
                },
            ),
            "FLIGHT_STATE/entity_id=2": record(
                "FLIGHT_STATE",
                {
                    "entity_id": "2",
                    "position_world": [300.0, 400.0, 0.0],
                    "velocity_world": [-30.0, -40.0, 0.0],
                    "orientation_local_to_world": [1.0, 0.0, 0.0, 0.0],
                },
            ),
        }

        def projection() -> dict[str, dict[str, object]]:
            return console.DashboardProjection(
                state, at_us=1_000_000
            ).build()["derived"]

        values = projection()
        self.assertEqual(0.25, values["entities.1.cargo.progress_ratio"]["value"])
        self.assertEqual(
            7_500_000.0, values["entities.1.cargo.remaining_us"]["value"]
        )
        self.assertEqual(500.0, values["entities.1.support.distance"]["value"])
        self.assertEqual(
            50.0, values["entities.1.support.relative_speed"]["value"]
        )
        self.assertEqual(
            50.0, values["entities.1.support.closing_speed"]["value"]
        )
        inventory = console.DashboardProjection(
            state, at_us=1_000_000
        ).build()["inventory"]
        closing_inventory = next(
            item for item in inventory
            if item["path"] == "entities.1.support.closing_speed"
        )
        cargo_inventory = next(
            item for item in inventory
            if item["path"] == "entities.1.cargo.progress_ratio"
        )
        self.assertEqual(
            "p2.dashboard.support-closing-speed.v1",
            closing_inventory["formulaId"],
        )
        self.assertIn(
            "wire:SUPPORT_STATE.support_entity_id",
            closing_inventory["provenance"],
        )
        self.assertEqual(
            "p2.dashboard.cargo-progress-ratio.v1",
            cargo_inventory["formulaId"],
        )
        self.assertFalse(values["dashboard.support_eta_us"]["available"])

        cargo = state.record_instances["CARGO_SCAN_STATE/entity_id=1"]["fields"]
        cargo.pop("required_us")
        missing_timing = projection()
        self.assertFalse(
            missing_timing["entities.1.cargo.progress_ratio"]["available"]
        )
        self.assertFalse(
            missing_timing["entities.1.cargo.remaining_us"]["available"]
        )

        support = state.record_instances["SUPPORT_STATE/entity_id=1"]["fields"]
        support.pop("support_entity_id")
        missing_support = projection()
        self.assertFalse(
            missing_support["entities.1.support.distance"]["available"]
        )
        self.assertEqual(
            "support-entity-absent",
            missing_support["entities.1.support.distance"]["reason"],
        )

        support["support_entity_id"] = "1"
        self_support = projection()
        self.assertEqual(0.0, self_support["entities.1.support.distance"]["value"])
        self.assertEqual(
            0.0, self_support["entities.1.support.relative_speed"]["value"]
        )
        self.assertEqual(
            0.0, self_support["entities.1.support.closing_speed"]["value"]
        )

    def test_phase3_tactical_dashboard_derivations_are_local_and_fail_closed(self) -> None:
        def record(
            name: str,
            fields: dict[str, object],
            version: int = 1,
        ) -> dict[str, object]:
            return {
                "recordName": name,
                "recordVersion": version,
                "fields": fields,
            }

        state = console.ConsoleState()
        state.record_instances = {
            "FLIGHT_STATE/entity_id=1": record(
                "FLIGHT_STATE",
                {
                    "entity_id": "1",
                    "position_world": [0.0, 0.0, 0.0],
                    "velocity_world": [0.0, 0.0, 0.0],
                    "orientation_local_to_world": [1.0, 0.0, 0.0, 0.0],
                },
            ),
            "RADAR_STATE/entity_id=1": record(
                "RADAR_STATE",
                {
                    "entity_id": "1",
                    "selected_range": 1_000.0,
                    "sensor_current_hits": 50.0,
                    "sensor_max_hits": 100.0,
                },
            ),
            "RADAR_CONTACTS/entity_id=1/contact_entity_id=101": record(
                "RADAR_CONTACTS",
                {
                    "entity_id": "1",
                    "contact_entity_id": "101",
                    "producer_sample_time_us": "900000",
                    "position_world": [0.0, 0.0, 500.0],
                    "velocity_world": [0.0, 0.0, -25.0],
                    "radar_local_position": [0.0, 0.0, 500.0],
                    "radar_projection_distance": 500.0,
                },
                version=2,
            ),
            "TARGET_STATE/entity_id=1": record(
                "TARGET_STATE",
                {
                    "entity_id": "1",
                    "current_target_entity_id": "101",
                    "exact_hud_distance": 0.0,
                },
            ),
            "WEAPON_STATE/entity_id=1": record(
                "WEAPON_STATE",
                {
                    "entity_id": "1",
                    "current_secondary_bank_id": 21,
                    "secondary_banks": [
                        {"bank_id": 21, "weapon_class_id": 201}
                    ],
                },
            ),
            "LOCK_STATE/entity_id=1": record(
                "LOCK_STATE",
                {
                    "entity_id": "1",
                    "locks": [
                        {
                            "locked": False,
                            "target_entity_id": "101",
                            "time_to_lock_remaining_us": "1000000",
                        }
                    ],
                },
            ),
            "THREAT_STATE/entity_id=1": record(
                "THREAT_STATE",
                {
                    "entity_id": "1",
                    "producer_sample_time_us": "900000",
                    "incoming_missiles": [
                        {
                            "entity_id": "202",
                            "position_world": [0.0, 0.0, 1_000.0],
                            "velocity_world": [0.0, 0.0, -100.0],
                        },
                        {
                            "entity_id": "203",
                            "position_world": [0.0, 0.0, 1_000.0],
                            "velocity_world": [0.0, 0.0, 100.0],
                        },
                    ],
                },
            ),
        }
        state.manifest_records = {
            "WEAPON_MANIFEST/weapon_class_id=201": record(
                "WEAPON_MANIFEST",
                {
                    "weapon_class_id": 201,
                    "lock": {"time_us": "2000000"},
                },
            ),
        }

        def projection() -> dict[str, dict[str, object]]:
            return console.DashboardProjection(
                state,
                at_us=1_000_000,
                smoothed_offset_us=0,
                offset_filter_valid=True,
            ).build()["derived"]

        values = projection()
        self.assertEqual(
            [0.0, 0.0, 500.0],
            values["entities.1.tracks.101.relative_position_local"]["value"],
        )
        self.assertEqual(
            [0.0, 0.0],
            values["entities.1.tracks.101.scope_position"]["value"],
        )
        self.assertTrue(values["entities.1.tracks.101.scope_in_range"]["value"])
        self.assertEqual(0.0, values["entities.1.tracks.101.bearing_local_rad"]["value"])
        self.assertEqual(0.0, values["entities.1.tracks.101.elevation_local_rad"]["value"])

        # The standard FSO radar is a directional disc: its centre is the
        # forward axis, with local right/left and up/down determining the
        # direction from that centre. Range filters a contact but never moves
        # its marker radially.
        track = state.record_instances[
            "RADAR_CONTACTS/entity_id=1/contact_entity_id=101"
        ]["fields"]
        track["radar_local_position"] = [1_000.0, 0.0, 0.0]
        track["radar_projection_distance"] = 1_000.0
        right = projection()
        self.assertEqual(
            [0.5, 0.0],
            right["entities.1.tracks.101.scope_position"]["value"],
        )
        track["radar_local_position"] = [0.0, 1_000.0, 0.0]
        above = projection()
        self.assertEqual(
            [0.0, -0.5],
            above["entities.1.tracks.101.scope_position"]["value"],
        )
        track["radar_local_position"] = [-500.0, -500.0, 0.0]
        track["radar_projection_distance"] = math.sqrt(500_000.0)
        left_below = projection()
        self.assertLess(
            left_below["entities.1.tracks.101.scope_position"]["value"][0],
            0.0,
        )
        self.assertGreater(
            left_below["entities.1.tracks.101.scope_position"]["value"][1],
            0.0,
        )
        track["radar_local_position"] = [10.0, 0.0, -1_000.0]
        track["radar_projection_distance"] = math.hypot(10.0, 1_000.0)
        behind = projection()
        self.assertAlmostEqual(
            0.9968,
            behind["entities.1.tracks.101.scope_position"]["value"][0],
            places=4,
        )
        self.assertEqual(
            0.0,
            behind["entities.1.tracks.101.scope_position"]["value"][1],
        )
        # A newer FLIGHT_STATE pose cannot move a v2 contact: the projection
        # is atomic inside RADAR_CONTACTS and publication already proves range.
        state.record_instances["FLIGHT_STATE/entity_id=1"]["fields"][
            "orientation_local_to_world"
        ] = [0.0, 0.0, 1.0, 0.0]
        self.assertEqual(
            behind["entities.1.tracks.101.scope_position"]["value"],
            projection()["entities.1.tracks.101.scope_position"]["value"],
        )
        self.assertTrue(
            projection()["entities.1.tracks.101.scope_in_range"]["value"]
        )
        track["radar_local_position"] = [0.0, 0.0, 500.0]
        track["radar_projection_distance"] = 500.0

        self.assertEqual(0.0, values["entities.1.target.distance"]["value"])
        self.assertEqual(0.5, values["entities.1.locks[0].progress"]["value"])
        self.assertEqual(1_000.0, values["entities.1.missiles.202.distance"]["value"])
        self.assertEqual(100.0, values["entities.1.missiles.202.closing_speed"]["value"])
        self.assertEqual(10.0, values["entities.1.missiles.202.ttc_s"]["value"])
        self.assertFalse(values["entities.1.missiles.203.ttc_s"]["available"])
        self.assertEqual(
            "missile-not-approaching-or-invalid",
            values["entities.1.missiles.203.ttc_s"]["reason"],
        )

        target = state.record_instances["TARGET_STATE/entity_id=1"]["fields"]
        target.pop("exact_hud_distance")
        self.assertEqual(500.0, projection()["entities.1.target.distance"]["value"])

        target_envelope = state.record_instances["TARGET_STATE/entity_id=1"]
        target_envelope["recordVersion"] = 2
        target["exact_hud_distance"] = 809.0
        target["exact_hud_speed"] = 92.0
        hud = projection()
        self.assertEqual(809.0, hud["entities.1.target.distance"]["value"])
        self.assertEqual(92.0, hud["entities.1.target.hud_speed"]["value"])
        self.assertEqual(
            "p3.dashboard.target-distance.v2",
            next(item["formulaId"] for item in console.DashboardProjection(
                state, at_us=1_000_000, smoothed_offset_us=0,
                offset_filter_valid=True,
            ).build()["inventory"] if item["path"] == "entities.1.target.distance"),
        )

        weapon = state.record_instances["WEAPON_STATE/entity_id=1"]["fields"]
        weapon["current_secondary_bank_id"] = 999
        ambiguous = projection()["entities.1.locks[0].progress"]
        self.assertFalse(ambiguous["available"])
        self.assertEqual(
            "selected-secondary-lock-manifest-unavailable",
            ambiguous["reason"],
        )

    def test_console_tool_exists_and_does_not_import_producer_cpp_bindings(self) -> None:
        self.assertTrue(
            CONSOLE.is_file(),
            "the independent observation client must remain available",
        )

        tree = ast.parse(CONSOLE.read_text(encoding="utf-8"), filename=str(CONSOLE))
        imported_roots: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported_roots.update(alias.name.split(".", 1)[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imported_roots.add(node.module.split(".", 1)[0])

        self.assertFalse(
            {"telemetry", "code"} & imported_roots,
            "D1-013: console must not import producer C++ parser/DTO bindings",
        )

    def test_reference_decoder_cross_checks_frozen_and_amendment_corpora(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "fstl_reference_decoder.py"), "--check", "--repo", str(REPO)],
            cwd=REPO, text=True, capture_output=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr + result.stdout)
        self.assertIn("22 FSTL 1.1 corpus cases cross-decoded", result.stdout)
        self.assertIn("CRC and simulated cross-endian checks passed", result.stdout)

    def test_phase1_independent_reader_keeps_decoding_known_v1_phase2_records(self) -> None:
        payload = v11_payload("phase2-promotion", ".bin")
        decoded = reference.decode_message(6, 0, payload, {})
        records = decoded["fields"]["records"]
        self.assertEqual(
            [
                "SESSION_STATE",
                "MISSION_STATE",
                "ENTITY_LIFECYCLE",
                "FLIGHT_STATE",
                "SHIP_IDENTITY",
                "DAMAGE_STATE",
                "SHIELD_STATE",
                "SUBSYSTEM_STATE",
                "ENERGY_STATE",
                "PROPULSION_STATE",
            ],
            [record["recordName"] for record in records],
        )
        self.assertTrue(all(1 <= int(record["recordType"]) <= 24 for record in records))
        self.assertEqual("None", reference.fstl11_snapshot_result(decoded, 1))

    def test_replay_never_publishes_snapshot_before_handshake(self) -> None:
        snapshot = packet(6, v11_payload("minimal-with-player", ".bin"),
                          session_id=0x1122334455667788, sequence=1, sent_us=1_000_000, flags=2)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "snapshot-before-session.bin"
            path.write_bytes(snapshot)
            result = subprocess.run([sys.executable, "-B", str(CONSOLE), "--replay", str(path), "--stale-ms", "1"],
                                    cwd=REPO, text=True, capture_output=True, check=False)
        self.assertNotEqual(0, result.returncode, "pre-handshake snapshot must be rejected")
        self.assertEqual("", result.stdout)

    def test_welcome_must_echo_hello_nonce_and_t0_before_any_ack(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.0)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.25"], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            bad = bytearray(welcome_for(hello)); bad[0] ^= 0x01  # client_nonce mismatch
            server.sendto(packet(3, bytes(bad), session_id=0x1122334455667788,
                                 sequence=1, sent_us=1_000_000, flags=2), client)
            controls: list[int] = []
            try:
                while True:
                    data, _ = server.recvfrom(1200)
                    controls.append(reference.read_header(data)["message_type"])
            except (socket.timeout, ConnectionResetError):
                pass
            stdout, stderr = process.communicate(timeout=3)

        self.assertNotEqual(0, process.returncode, "mismatched WELCOME is a failed negotiation")
        self.assertNotIn(10, controls, "a mismatched WELCOME must not be ACKed")
        self.assertEqual("", stdout)

    def test_lost_hello_retransmits_same_logical_message_with_retransmission_flag(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.5)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.60"], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            first, _ = server.recvfrom(1200)
            second, _ = server.recvfrom(1200)
            stdout, stderr = process.communicate(timeout=3)

        first_header, second_header = reference.read_header(first), reference.read_header(second)
        self.assertEqual(0, process.returncode, stderr + stdout)
        self.assertEqual((2, 0), (first_header["message_type"], first_header["session_id"]))
        # Decode the actual ConsoleClient HELLO and independently validate its
        # complete transport envelope, including both CRC layers.
        decoded_hello = reference.decode_message(
            first_header["message_type"], first_header["flags"], first[68:], {}
        )
        envelope = reference.decode_transport_sequence(
            [first], "console-hello", {"acceptedMinorRange": [1, 1]}
        )
        self.assertEqual("HELLO", decoded_hello["messageName"])
        self.assertEqual(1, envelope["fragmentCount"])
        self.assertEqual(len(first) - 68, envelope["messageSize"])
        self.assertEqual(0, first_header["flags"] & 0x02, "HELLO must not request an ACK")
        self.assertEqual(first_header["message_id"], second_header["message_id"])
        self.assertEqual(first[68:], second[68:], "HELLO retransmission retains nonce/t0/payload")
        self.assertEqual(0, first_header["flags"] & 0x10)
        self.assertEqual(0, second_header["flags"] & 0x02, "HELLO retry must not request an ACK")
        self.assertNotEqual(0, second_header["flags"] & 0x10, "retry carries RETRANSMISSION")
        self.assertGreater(second_header["packet_sequence"], first_header["packet_sequence"])

    def test_retransmitted_welcome_and_session_begin_are_applied_once_and_acked_idempotently(self) -> None:
        class Sender:
            def __init__(self) -> None:
                self.packets: list[bytes] = []

            def send(self, packet_bytes: bytes) -> None:
                self.packets.append(packet_bytes)

        sender = Sender()
        client = console.ConsoleClient(sender, 3_000_000)
        client.begin()
        hello = sender.packets.pop()
        session_id = 0x1122334455667788
        welcome_payload = welcome_for(hello)
        begin_payload = session_begin_payload()

        def deliver(
            message_type: int,
            payload: bytes,
            sequence: int,
            message_id: int,
            flags: int,
        ) -> bool:
            return client.receive(
                packet(
                    message_type,
                    payload,
                    session_id=session_id,
                    sequence=sequence,
                    sent_us=1_000_000 + sequence,
                    message_id=message_id,
                    flags=flags,
                ),
                2_000_000 + sequence,
                "2026-08-02T12:00:00.000000Z",
            )

        self.assertTrue(deliver(3, welcome_payload, 1, 1, console.ACK_REQUIRED))
        self.assertTrue(deliver(4, begin_payload, 2, 2, console.ACK_REQUIRED))
        self.assertFalse(
            deliver(
                3,
                welcome_payload,
                3,
                1,
                console.ACK_REQUIRED | console.RETRANSMISSION,
            )
        )
        self.assertFalse(
            deliver(
                4,
                begin_payload,
                4,
                2,
                console.ACK_REQUIRED | console.RETRANSMISSION,
            )
        )
        self.assertTrue(client.state.session_begun)
        acknowledgements = [
            reference.decode_message(
                10,
                reference.read_header(ack)["flags"],
                ack[68:],
                {},
            )["fields"]
            for ack in sender.packets
            if reference.read_header(ack)["message_type"] == 10
        ]
        self.assertEqual(4, len(acknowledgements))
        self.assertTrue(all(
            fields["ack_flags"] == console.ACK_APPLIED
            for fields in acknowledgements
        ))
        self.assertEqual(
            [1, 2, 1, 2],
            [fields["target_message_id"] for fields in acknowledgements],
        )

    def test_heartbeat_requests_receive_correlated_responses_and_keep_the_udp_session_alive(self) -> None:
        session_id = 0x1122334455667788
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(2.0)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "13.0", "--stale-ms", "100000000"], cwd=REPO, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            server.sendto(packet(3, welcome_for(hello), session_id=session_id,
                                 sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id,
                                 sequence=2, sent_us=1_000_001, flags=2), client)

            def request_and_assert_response(probe_id: int, origin_t0_us: int) -> None:
                server.sendto(packet(9, heartbeat_request_payload(probe_id, origin_t0_us), session_id=session_id,
                                     sequence=probe_id, sent_us=origin_t0_us, flags=0), client)
                response, response_client = receive_message_type(server, 9)
                self.assertEqual(client, response_client)
                header = reference.read_header(response)
                envelope = reference.decode_transport_sequence(
                    [response], "console-heartbeat-response", {"acceptedMinorRange": [1, 1]}
                )
                decoded = reference.decode_message(9, header["flags"], response[68:], {})["fields"]
                self.assertEqual(0, header["flags"])
                self.assertEqual(session_id, header["session_id"])
                self.assertEqual(1, header["fragment_count"])
                self.assertEqual(header["packet_sequence"], header["message_id"])
                self.assertEqual(header["sent_time_us"], int(decoded["transmit_t2_us"]))
                self.assertEqual(1, envelope["fragmentCount"])
                self.assertEqual(probe_id, decoded["probe_id"])
                self.assertEqual(2, decoded["kind"])
                self.assertEqual(str(origin_t0_us), decoded["origin_t0_us"])
                self.assertGreaterEqual(int(decoded["transmit_t2_us"]), int(decoded["receive_t1_us"]))

            request_and_assert_response(1, 1_100_000)
            # The producer's minimum disconnect timeout is 10 seconds.  A
            # second correlated response before that deadline keeps the same
            # negotiated UDP session alive past its initial timeout horizon.
            time.sleep(9.25)
            request_and_assert_response(2, 10_350_000)
            time.sleep(1.0)
            request_and_assert_response(3, 11_400_000)
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)

    def test_resync_retransmits_until_validated_ack_then_enters_synchronizing(self) -> None:
        session_id = 0x1122334455667788
        unknown_payload = bytearray(v11_payload("delta-player-return-baseline", ".payload.bin"))
        struct.pack_into("<I", unknown_payload, 0, 999)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.5)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "1.10", "--stale-ms", "100000000"], cwd=REPO, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            server.sendto(packet(3, welcome_for(hello), session_id=session_id, sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2), client)
            server.sendto(packet(6, v11_payload("minimal-with-player", ".bin"), session_id=session_id,
                                 sequence=3, sent_us=1_000_002, flags=2), client)
            server.sendto(packet(7, bytes(unknown_payload), session_id=session_id,
                                 sequence=4, sent_us=1_000_003, flags=2), client)
            resync_headers: list[dict[str, int]] = []
            while len(resync_headers) < 2:
                data, _ = server.recvfrom(1200)
                header = reference.read_header(data)
                if header["message_type"] == 12:
                    resync_headers.append(header)
            self.assertEqual(resync_headers[0]["message_id"], resync_headers[1]["message_id"])
            self.assertEqual(resync_headers[0]["message_crc32"], resync_headers[1]["message_crc32"])
            self.assertEqual(0, resync_headers[0]["flags"] & 0x10)
            self.assertNotEqual(0, resync_headers[1]["flags"] & 0x10)
            wrong_crc = bytearray(ack_for(resync_headers[1])); wrong_crc[8] ^= 0x01
            wrong_count = bytearray(ack_for(resync_headers[1])); struct.pack_into("<H", wrong_count, 6, 2)
            server.sendto(packet(10, bytes(wrong_crc), session_id=session_id, sequence=5, sent_us=1_000_004), client)
            server.sendto(packet(10, bytes(wrong_count), session_id=session_id, sequence=6, sent_us=1_000_005), client)
            while len(resync_headers) < 3:
                data, _ = server.recvfrom(1200)
                header = reference.read_header(data)
                if header["message_type"] == 12:
                    resync_headers.append(header)
            self.assertEqual(resync_headers[1]["message_id"], resync_headers[2]["message_id"],
                             "wrong CRC/count ACKs must not clear pending resync")
            server.sendto(packet(10, ack_for(resync_headers[2]), session_id=session_id,
                                 sequence=7, sent_us=1_000_006), client)
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)
        states = [json.loads(line)["status"] for line in stdout.splitlines()]
        self.assertIn("Live", states)
        self.assertEqual("Synchronizing", states[-1], "only inbound VALIDATED ACK promotes Stale to Synchronizing")

    def test_replay_nominal_handshake_resync_and_shutdown_is_deterministic(self) -> None:
        """Replay drives the same successful state machine as live UDP, not reject-only paths."""
        session_id = 0x1122334455667788
        unknown_payload = bytearray(v11_payload("delta-player-return-baseline", ".payload.bin"))
        struct.pack_into("<I", unknown_payload, 0, 999)
        packets = (
            packet(3, v11_payload("welcome-accepted-minor-one", ".payload.bin"), session_id=session_id,
                   sequence=1, sent_us=1_000_000, flags=2),
            packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2),
            packet(6, v11_payload("minimal-with-player", ".bin"), session_id=session_id,
                   sequence=3, sent_us=1_000_002, flags=2),
            packet(7, bytes(unknown_payload), session_id=session_id, sequence=4, sent_us=1_000_003),
            packet(13, session_end_payload(), session_id=session_id, sequence=5, sent_us=1_000_004, flags=2),
        )
        with tempfile.TemporaryDirectory() as temp:
            paths: list[str] = []
            for index, value in enumerate(packets):
                path = Path(temp) / f"nominal-{index}.bin"; path.write_bytes(value); paths.append(str(path))
            command = [sys.executable, "-B", str(CONSOLE), "--replay", *paths, "--stale-ms", "100000000"]
            first = subprocess.run(command, cwd=REPO, text=True, capture_output=True, check=False)
            second = subprocess.run(command, cwd=REPO, text=True, capture_output=True, check=False)
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(first.stdout, second.stdout, "nominal replay transcript must be byte-for-byte deterministic")
        transcript = [json.loads(line) for line in first.stdout.splitlines()]
        states = [line["status"] for line in transcript]
        self.assertEqual(["Synchronizing", "Synchronizing", "Live", "Stale", "Disconnected"], states)
        for line in transcript:
            self.assertEqual("replay-simulated", line["observation_clock"])
            self.assertRegex(line["observed_at_utc"], r"^1970-01-01T00:00:\d\d\.\d{6}Z$")
            self.assertIsInstance(int(line["observed_at_monotonic_us"]), int)
        protocol_stale = next(line for line in transcript if line["status"] == "Stale")
        self.assertEqual("protocol-resync", protocol_stale["stale_reason"])
        self.assertIsNone(protocol_stale["stale_detected_monotonic_us"])
        self.assertIsNone(protocol_stale["stale_detected_utc"])
        self.assertIsNone(protocol_stale["stale_duration_us"])

    def test_drop_once_delta_is_reported_and_next_cumulative_delta_converges(self) -> None:
        session_id = 0x1122334455667788
        packets = (
            packet(3, v11_payload("welcome-accepted-minor-one", ".payload.bin"),
                   session_id=session_id, sequence=1, sent_us=1_000_000, flags=2),
            packet(4, session_begin_payload(), session_id=session_id,
                   sequence=2, sent_us=1_000_001, flags=2),
            packet(6, v11_payload("minimal-with-player", ".bin"),
                   session_id=session_id, sequence=3, sent_us=1_000_002, flags=2),
            packet(7, v11_payload("delta-player-kinematics-cumulative", ".payload.bin"),
                   session_id=session_id, sequence=4, sent_us=1_000_003),
            packet(7, v11_payload("delta-player-return-baseline", ".payload.bin"),
                   session_id=session_id, sequence=5, sent_us=1_000_004),
        )
        with tempfile.TemporaryDirectory() as temp:
            paths: list[str] = []
            for index, value in enumerate(packets):
                path = Path(temp) / f"drop-once-{index}.bin"
                path.write_bytes(value)
                paths.append(str(path))
            result = subprocess.run(
                [sys.executable, "-B", str(CONSOLE), "--drop-once", "delta",
                 "--replay", *paths, "--stale-ms", "100000000"],
                cwd=REPO, text=True, capture_output=True, check=False,
            )

        self.assertEqual(0, result.returncode, result.stderr)
        transcripts = [json.loads(line) for line in result.stdout.splitlines()]
        injection = next(line for line in transcripts if line["injections"])
        self.assertEqual(
            [{"action": "drop-once", "message": "delta", "message_id": 4}],
            injection["injections"],
        )
        self.assertEqual(0, injection["delta_sequence"])
        self.assertEqual(3, next(
            line["delta_sequence"] for line in transcripts
            if line["delta_sequence"] == 3
        ))

    def test_unknown_baseline_stays_stale_without_resync_ack(self) -> None:
        session_id = 0x1122334455667788
        unknown_payload = bytearray(v11_payload("delta-player-return-baseline", ".payload.bin"))
        struct.pack_into("<I", unknown_payload, 0, 999)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.5)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.65", "--stale-ms", "100000000"], cwd=REPO, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            server.sendto(packet(3, welcome_for(hello), session_id=session_id, sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2), client)
            server.sendto(packet(6, v11_payload("minimal-with-player", ".bin"), session_id=session_id,
                                 sequence=3, sent_us=1_000_002, flags=2), client)
            server.sendto(packet(7, bytes(unknown_payload), session_id=session_id,
                                 sequence=4, sent_us=1_000_003, flags=2), client)
            controls: list[int] = []
            deadline = time.monotonic() + 0.45
            while time.monotonic() < deadline:
                try:
                    data, _ = server.recvfrom(1200)
                except (socket.timeout, ConnectionResetError):
                    break
                controls.append(reference.read_header(data)["message_type"])
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)
        self.assertIn(12, controls, "unknown baseline starts reliable resync")
        states = [json.loads(line)["status"] for line in stdout.splitlines()]
        self.assertIn("Live", states)
        self.assertEqual("Stale", states[-1], "no ACK must not prematurely enter Synchronizing")

    def test_snapshot_declared_and_aggregate_candidate_quotas_are_fail_closed(self) -> None:
        """16 MiB per transaction and 32 MiB aggregate are checked before promotion."""
        first, _ = snapshot_parts()
        declared_over_limit = with_snapshot_transaction_size(first, 16 * 1024 * 1024 + 1)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "over-16m.bin"
            path.write_bytes(packet(6, declared_over_limit, session_id=0x1122334455667788,
                                    sequence=1, sent_us=1_000_000, flags=2))
            result = subprocess.run([sys.executable, "-B", str(CONSOLE), "--replay", str(path)],
                                    cwd=REPO, text=True, capture_output=True, check=False)
        self.assertNotEqual(0, result.returncode, "a declared snapshot above 16 MiB must be rejected")
        self.assertEqual("", result.stdout)

        candidates = [with_snapshot_transaction_size(first, 16 * 1024 * 1024, snapshot_id=index)
                      for index in (11, 12, 13)]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.0)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.40"], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            session_id = 0x1122334455667788
            server.sendto(packet(3, welcome_for(hello), session_id=session_id, sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2), client)
            for sequence, payload in enumerate(candidates, start=3):
                server.sendto(packet(6, payload, session_id=session_id, sequence=sequence,
                                     sent_us=1_000_000 + sequence, flags=2), client)
            stdout, stderr = process.communicate(timeout=3)
        self.assertNotEqual(0, process.returncode, "a third 16 MiB candidate exceeds the 32 MiB aggregate quota")
        self.assertNotIn('"status":"Live"', stdout, "candidate reservations cannot publish a partial snapshot")

    def test_udp_handshake_transaction_resync_and_terminal_semantics(self) -> None:
        """Exact P0 ordering: Hello/Welcome/SessionBegin before atomic snapshot."""
        session_id = 0x1122334455667788
        snapshot_a, snapshot_b = snapshot_parts()
        changed_payload = v11_payload("delta-player-kinematics-cumulative", ".payload.bin")
        unknown_payload = bytearray(v11_payload("delta-player-return-baseline", ".payload.bin"))
        struct.pack_into("<I", unknown_payload, 0, 999)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(1.0)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.8", "--stale-ms", "5"], cwd=REPO, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            self.assertEqual(2, reference.read_header(hello)["message_type"])
            server.sendto(packet(3, welcome_for(hello), session_id=session_id, sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2), client)
            server.sendto(packet(6, snapshot_a, session_id=session_id, sequence=3, sent_us=1_000_002, flags=2), client)
            server.sendto(packet(6, snapshot_b, session_id=session_id, sequence=4, sent_us=1_000_003, flags=2), client)
            server.sendto(packet(7, changed_payload, session_id=session_id, sequence=5, sent_us=1_000_004), client)
            server.sendto(packet(7, v11_payload("delta-player-return-baseline", ".payload.bin"),
                                 session_id=session_id, sequence=6, sent_us=1_000_005), client)
            # A repeated older sequence must not roll the immutable baseline state backwards.
            server.sendto(packet(7, changed_payload, session_id=session_id, sequence=7, sent_us=1_000_006), client)
            server.sendto(packet(7, bytes(unknown_payload), session_id=session_id, sequence=8,
                                 sent_us=1_000_007, flags=2), client)
            server.sendto(packet(13, session_end_payload(), session_id=session_id, sequence=9, sent_us=1_000_008, flags=2), client)
            server.sendto(packet(13, session_end_payload(), session_id=session_id, sequence=10, message_id=9,
                                 sent_us=1_000_009, flags=2), client)
            received: list[tuple[int, dict[str, object]]] = []
            deadline = time.monotonic() + 0.7
            while time.monotonic() < deadline:
                try:
                    data, _ = server.recvfrom(1200)
                except (socket.timeout, ConnectionResetError):
                    break
                header = reference.read_header(data)
                received.append((header["message_type"], reference.decode_message(header["message_type"], header["flags"], data[68:], {})))
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)
        acks = [decoded["fields"] for kind, decoded in received if kind == 10]
        snapshot_acks = [fields["ack_flags"] for fields in acks if fields["target_message_type"] == 6]
        self.assertEqual([1, 1, 3, 3], snapshot_acks,
                         "each snapshot part is VALIDATED, then both become APPLIED only after one atomic commit")
        self.assertEqual(0, sum(1 for fields in acks if fields["target_message_type"] == 7),
                         "all Deltas are replaceable: hostile ACK_REQUIRED must never induce an ACK")
        self.assertGreaterEqual(sum(1 for kind, _ in received if kind == 12), 1,
                                "unknown baseline triggers a retained ResyncRequest")
        lines = [json.loads(line) for line in stdout.splitlines()]
        self.assertIn("Live", [line["status"] for line in lines])
        terminal = [line for line in lines if line["status"] == "Disconnected"]
        self.assertEqual(1, len(terminal), "duplicate SessionEnd is tombstoned without a second terminal publication")
        live = [line for line in lines if line["status"] == "Live"][-1]
        self.assertEqual([0.0, 0.0, 0.0], live["angular_velocity"])
        self.assertEqual([0.0, 0.0, 0.0], live["pose"]["position"])
        live_positions = [line["pose"]["position"] for line in lines if line["status"] == "Live"]
        self.assertEqual([[0.0, 0.0, 0.0], [10.0, 20.0, 30.0], [0.0, 0.0, 0.0]], live_positions,
                         "only strictly newer cumulative deltas may alter the immutable-baseline replica")

    def test_snapshot_hash_and_cross_part_mismatch_never_commit(self) -> None:
        """A candidate transaction is atomic and cannot be promoted by a bad second part."""
        first, second = snapshot_parts()
        bad = bytearray(second); bad[12] ^= 0x80  # transaction_sha256 differs across parts
        with tempfile.TemporaryDirectory() as temp:
            paths: list[str] = []
            for index, payload in enumerate((first, bytes(bad))):
                path = Path(temp) / f"part-{index}.bin"
                path.write_bytes(packet(6, payload, session_id=0x1122334455667788,
                                        sequence=index + 1, sent_us=1_000_000 + index, flags=2))
                paths.append(str(path))
            result = subprocess.run([sys.executable, "-B", str(CONSOLE), "--replay", *paths, "--stale-ms", "1"],
                                    cwd=REPO, text=True, capture_output=True, check=False)
        self.assertNotEqual(0, result.returncode, "cross-part hash mismatch must be rejected")
        self.assertEqual("", result.stdout, "bad transaction never publishes partial snapshot")

    def test_abrupt_peer_silence_becomes_stale_after_live_snapshot(self) -> None:
        """A peer disappearing without SESSION_END is a bounded Stale transition."""
        session_id = 0x1122334455667788
        snapshot = packet(6, v11_payload("minimal-with-player", ".bin"), session_id=session_id,
                          sequence=3, sent_us=1_000_002, flags=2)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0)); server.settimeout(2.0)
            process = subprocess.Popen(
                [sys.executable, "-B", str(CONSOLE), "--host", "127.0.0.1", "--port", str(server.getsockname()[1]),
                 "--seconds", "0.30", "--stale-ms", "1"], cwd=REPO, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            hello, client = server.recvfrom(1200)
            server.sendto(packet(3, welcome_for(hello), session_id=session_id, sequence=1, sent_us=1_000_000, flags=2), client)
            server.sendto(packet(4, session_begin_payload(), session_id=session_id, sequence=2, sent_us=1_000_001, flags=2), client)
            server.sendto(snapshot, client)
            stdout, stderr = process.communicate(timeout=3)

        self.assertEqual(0, process.returncode, stderr + stdout)
        transcript = [json.loads(line) for line in stdout.splitlines()]
        states = [line["status"] for line in transcript]
        self.assertIn("Live", states)
        self.assertEqual("Stale", states[-1])
        stale = transcript[-1]
        self.assertEqual("local", stale["observation_clock"])
        self.assertRegex(stale["observed_at_utc"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$")
        self.assertIsNotNone(stale["last_live_observed_monotonic_us"])
        self.assertIsNotNone(stale["last_live_observed_utc"])
        self.assertGreater(int(stale["last_live_age_us"]), 1_000)
        self.assertEqual("silence", stale["stale_reason"])
        self.assertIsNotNone(stale["stale_detected_monotonic_us"])
        self.assertIsNotNone(stale["stale_detected_utc"])
        self.assertEqual(
            int(stale["observed_at_monotonic_us"]) - int(stale["last_live_observed_monotonic_us"]),
            int(stale["last_live_age_us"]),
            "the final Stale record must quantify local silence since the last Live observation",
        )
        self.assertEqual(
            int(stale["observed_at_monotonic_us"]) - int(stale["stale_detected_monotonic_us"]),
            int(stale["stale_duration_us"]),
            "the final Stale record must retain the actual timer transition and its elapsed Stale duration",
        )


if __name__ == "__main__":
    unittest.main()

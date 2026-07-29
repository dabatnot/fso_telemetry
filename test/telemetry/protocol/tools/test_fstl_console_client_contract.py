#!/usr/bin/env python3
"""WP10 structural contract for the independent Phase 1 console proof tool.

The behavioural UDP transcript remains intentionally black-box: the Phase 1
contract defines its observable behaviour but not a Python API.  This guard
keeps the tool location and the D1-013 dependency boundary explicit before
the integration fixture supplies packets to it.
"""

from __future__ import annotations

import ast
import importlib.util
import json
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
REPO = TOOLS.parents[3]
V11 = REPO / "test" / "telemetry" / "protocol" / "vectors-v1.1"

sys.path.insert(0, str(TOOLS))
import fstl_reference_decoder as reference
import fstl_console_client as console


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
        ), "P2-AC-006: every raw field has non-empty wire provenance")

        without_clock = console.DashboardProjection(state, at_us=1_100_000).build()
        age = next(item for item in without_clock["inventory"]
                   if item["path"] == "records.FLIGHT_STATE/entity_id=1.age_us")
        self.assertFalse(age["available"])
        self.assertEqual("invalid-or-missing-clock-offset", age["reason"])

    def test_phase2_oracle_goldens_are_canonical_black_box_transcripts(self) -> None:
        generator = REPO / "test/telemetry/producer/phase2/generate_phase2_oracle_goldens.py"
        result = subprocess.run(
            [sys.executable, "-B", str(generator)],
            cwd=REPO, text=True, capture_output=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr + result.stdout)
        self.assertIn("inventories are canonical", result.stdout)

    def test_phase2_oracle_inventory_comparison_requires_provenance_and_formula(self) -> None:
        runner = REPO / "test/telemetry/producer/phase2/run_phase2_oracle_evidence.py"
        spec = importlib.util.spec_from_file_location("phase2_oracle_runner", runner)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        missing_source = [{"kind": "A", "path": "a", "available": True, "value": 1}]
        _, raw_summary = module.compare_inventory(missing_source, missing_source)
        self.assertEqual(1, raw_summary["missingProvenance"])
        self.assertEqual(1, raw_summary["mismatches"])

        missing_formula = [{
            "kind": "D", "path": "d", "available": True, "value": 1,
            "provenance": "oracle:double",
        }]
        _, derived_summary = module.compare_inventory(missing_formula, missing_formula)
        self.assertEqual(1, derived_summary["uncoveredFormula"])
        self.assertEqual(1, derived_summary["mismatches"])

    def test_phase2_oracle_runner_missing_harness_fails_closed_without_report(self) -> None:
        runner = REPO / "test/telemetry/producer/phase2/run_phase2_oracle_evidence.py"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / "report"
            result = subprocess.run(
                [
                    sys.executable, "-B", str(runner),
                    "--profiles", "core-gate", "complete-ship",
                    "--build-dir", str(root / "missing-build"),
                    "--config", "Release",
                    "--report-dir", str(report),
                ],
                cwd=REPO, text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("missing telemetry_phase2_oracle_harness", result.stderr)
            self.assertFalse(
                (report / "phase2-oracle-evidence.json").exists(),
                "fail-closed diagnostics must not leave a conclusive report",
            )

    def test_console_tool_exists_and_does_not_import_producer_cpp_bindings(self) -> None:
        self.assertTrue(
            CONSOLE.is_file(),
            "P1-WP-10 requires test/telemetry/protocol/tools/fstl_console_client.py",
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

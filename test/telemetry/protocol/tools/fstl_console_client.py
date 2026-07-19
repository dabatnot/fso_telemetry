#!/usr/bin/env python3
"""Small independent FSTL 1.1 proof client (P1-WP-10).

It deliberately imports only :mod:`fstl_reference_decoder`; it never imports
the C++ producer, its DTOs, fixture generators, or a generated layout.  The
``--replay`` mode is useful to produce deterministic transcripts in CI while
the default mode is a bounded UDP proof client for a configured producer.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import secrets
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Support both direct execution and black-box/imported contract harnesses while
# keeping the only protocol dependency adjacent and explicit.
_TOOLS_DIR = str(Path(__file__).resolve().parent)
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)
import fstl_reference_decoder as reference


MAGIC = 0x4C545346
HEADER_SIZE = 68
MAX_DATAGRAM = 1200
MAX_STATE_MESSAGE = 1_048_576
MAX_FRAGMENTS = 1024
MAX_SNAPSHOT_TRANSACTIONS = 2
MAX_SNAPSHOT_TRANSACTION_BYTES = 16_777_216
MAX_SNAPSHOT_CANDIDATE_BYTES = 33_554_432
ACK_REQUIRED = 0x02
ACK_VALIDATED = 0x01
# Phase 0 §9.7: Applied is meaningful only together with Validated.
ACK_APPLIED = ACK_VALIDATED | 0x02
RESYNC_UNKNOWN_BASELINE = 1
RESYNC_REQUIRE_FULL_SNAPSHOT = 0x02
SESSION_END_TOMBSTONE_US = 7_000_000
RELIABLE_WINDOW_US = 5_000_000
DEFAULT_RTO_US = 250_000
MAX_RTO_US = 1_000_000
RETRANSMISSION = 0x10


def now_us() -> int:
    return time.monotonic_ns() // 1000


def pack_header(*, message_type: int, flags: int, session_id: int,
                sequence: int, sent_us: int, message_id: int,
                payload: bytes, minor: int = 1) -> bytes:
    """Encode one unfragmented FSTL datagram with an independent CRC."""
    if len(payload) > reference.MAX_FRAGMENT_PAYLOAD:
        raise ValueError("control payload exceeds one FSTL datagram")
    crc = reference.crc32_iso_hdlc(payload)
    prefix = struct.pack(
        "<IBBBBHHQIIqQIHHIII",
        MAGIC, 1, minor, message_type, flags, HEADER_SIZE, len(payload),
        session_id, sequence, 0, 0, sent_us, message_id, 0, 1,
        len(payload), 0, crc,
    )
    # The final CRC seals bytes 0..63 with this field zeroed plus payload.
    checksum = reference.crc32_iso_hdlc(prefix + bytes(4) + payload)
    return prefix + struct.pack("<I", checksum) + payload


def hello_payload(nonce: int, sent_us: int) -> bytes:
    # HELLO prefix from the frozen wire contract: version 1.1 only, Cockpit.
    return struct.pack("<QQBBBBB3xQHHH", nonce, sent_us, 1, 1, 1, 1, 0, 0, 1000, 0, 0)


def heartbeat_payload(probe_id: int, kind: int, origin_t0_us: int,
                      receive_t1_us: int, transmit_t2_us: int) -> bytes:
    return struct.pack("<IB3xQQQ", probe_id, kind, origin_t0_us,
                       receive_t1_us, transmit_t2_us)


def ack_payload(header: dict[str, int], ack_flags: int) -> bytes:
    return struct.pack("<IBBHI", header["message_id"], header["message_type"],
                       ack_flags, header["fragment_count"],
                       header["message_crc32"])


def resync_payload(request_id: int, baseline: int, delta: int, sent_us: int) -> bytes:
    return struct.pack("<IBBHIIQ", request_id, RESYNC_UNKNOWN_BASELINE,
                       RESYNC_REQUIRE_FULL_SNAPSHOT, 0, baseline, delta, sent_us)


@dataclass
class FragmentSet:
    header: dict[str, int]
    pieces: dict[int, bytes] = field(default_factory=dict)


@dataclass
class PendingReliable:
    message_type: int
    message_id: int
    session_id: int
    payload: bytes
    fragment_count: int
    message_crc32: int
    first_us: int
    next_us: int
    attempt: int = 0


@dataclass
class ConsoleState:
    status: str = "Synchronizing"
    session_id: int = 0
    baseline: int = 0
    delta_sequence: int = 0
    last_state_us: int | None = None
    records: dict[str, dict[str, Any]] = field(default_factory=dict)
    baseline_records: dict[str, dict[str, Any]] = field(default_factory=dict)
    transactions: dict[int, dict[str, Any]] = field(default_factory=dict)
    hello_sent: bool = False
    hello_nonce: int | None = None
    hello_t0_us: int | None = None
    welcomed: bool = False
    session_begun: bool = False
    ended: bool = False

    def _apply_records(self, records: list[dict[str, Any]], replace: bool) -> None:
        if replace:
            self.records = {}
        for record in records:
            self.records[record["recordName"]] = record["fields"]

    def end_session(self) -> None:
        """Publish a terminal tombstone then release all retained heavy state."""
        # A received terminal record is an explicit clean disconnect.  ``Stale``
        # remains reserved for abrupt silence past the configured age limit.
        self.status = "Disconnected"
        self.baseline = self.delta_sequence = 0
        self.last_state_us = None
        self.records.clear()
        self.baseline_records.clear()
        self.transactions.clear()
        self.session_begun = False
        self.welcomed = False
        self.session_id = 0
        self.ended = True

    def apply(self, message_type: int, fields: dict[str, Any], payload: bytes,
              header: dict[str, int], at_us: int) -> tuple[bool, list[dict[str, int]], list[dict[str, int]]]:
        """Apply a validated producer message and return publish/ACK outcomes.

        Snapshot parts are ACK VALIDATED when reserved. ACK APPLIED headers are
        returned only after an atomic state or lifecycle publication.
        """
        if message_type == 3:  # WELCOME
            if (not self.hello_sent or self.hello_nonce is None or self.hello_t0_us is None or
                    fields["status"] != 0 or fields["selected_major"] != 1 or fields["selected_minor"] != 1 or
                    fields["client_nonce"] != str(self.hello_nonce) or fields["client_send_t0_us"] != str(self.hello_t0_us)):
                raise ValueError("WELCOME outside FSTL 1.1 negotiation")
            if header["session_id"] == 0:
                raise ValueError("WELCOME without session")
            self.session_id, self.welcomed, self.ended = header["session_id"], True, False
            return True, [], [header]
        if message_type == 4:  # SESSION_BEGIN
            if not self.welcomed or self.session_begun or header["session_id"] != self.session_id:
                raise ValueError("SESSION_BEGIN outside negotiated session")
            self.session_begun = True
            return True, [], [header]
        if message_type == 13:  # SESSION_END
            if not self.session_begun or header["session_id"] != self.session_id:
                raise ValueError("SESSION_END outside active session")
            self.end_session()
            return True, [], [header]
        if not self.session_begun or header["session_id"] != self.session_id:
            raise ValueError("state before negotiated SESSION_BEGIN")
        if message_type == 6:
            snapshot = fields["snapshot_id"]
            count = fields["part_count"]
            index = fields["part_index"]
            if not 1 <= count <= 64 or index >= count or fields["transaction_size"] > MAX_SNAPSHOT_TRANSACTION_BYTES:
                raise ValueError("invalid snapshot transaction bounds")
            candidate = self.transactions.get(snapshot)
            if candidate is None:
                if len(self.transactions) >= MAX_SNAPSHOT_TRANSACTIONS:
                    raise ValueError("snapshot transaction quota")
                candidate = {"part_count": count, "transaction_size": fields["transaction_size"],
                             "transaction_sha256": fields["transaction_sha256"],
                             "producer_sample_time_us": fields["producer_sample_time_us"],
                             "snapshot_flags": fields["snapshot_flags"],
                             "required_manifest_id": fields["required_manifest_id"], "parts": {}, "headers": {}}
                self.transactions[snapshot] = candidate
            for key in ("part_count", "transaction_size", "transaction_sha256", "producer_sample_time_us", "snapshot_flags", "required_manifest_id"):
                if candidate[key] != fields[key]:
                    self.transactions.pop(snapshot, None)
                    raise ValueError("inconsistent snapshot transaction")
            records_bytes = payload[60:]
            existing = candidate["parts"].get(index)
            if existing is not None and existing["records_bytes"] != records_bytes:
                self.transactions.pop(snapshot, None)
                raise ValueError("conflicting snapshot part")
            candidate["parts"][index] = {"fields": fields, "records_bytes": records_bytes}
            candidate["headers"][index] = header
            if sum(len(part["records_bytes"]) for part in candidate["parts"].values()) > candidate["transaction_size"]:
                self.transactions.pop(snapshot, None)
                raise ValueError("snapshot transaction size")
            if sum(sum(len(part["records_bytes"]) for part in item["parts"].values())
                   for item in self.transactions.values()) > MAX_SNAPSHOT_CANDIDATE_BYTES:
                self.transactions.pop(snapshot, None)
                raise ValueError("snapshot candidate byte quota")
            if len(candidate["parts"]) != count:
                return False, [header], []
            merged: list[dict[str, Any]] = []
            for part in range(count):
                if part not in candidate["parts"]:
                    return False, [header], []
                merged.extend(candidate["parts"][part]["fields"]["records"])
            concatenated = b"".join(candidate["parts"][part]["records_bytes"] for part in range(count))
            if len(concatenated) != candidate["transaction_size"] or hashlib.sha256(concatenated).hexdigest() != candidate["transaction_sha256"]:
                self.transactions.pop(snapshot, None)
                raise ValueError("snapshot transaction hash")
            names = {item["recordName"] for item in merged}
            if not {"SESSION_STATE", "MISSION_STATE"} <= names:
                raise ValueError("incomplete atomic snapshot")
            self._apply_records(merged, True)
            self.baseline = snapshot
            self.baseline_records = copy.deepcopy(self.records)
            self.delta_sequence = 0
            self.last_state_us = at_us
            self.status = "Live"
            ack_headers = [candidate["headers"][part] for part in range(count)]
            self.transactions.clear()
            return True, [header], ack_headers
        if message_type == 7:
            if fields["baseline_snapshot_id"] != self.baseline or fields["delta_sequence"] <= self.delta_sequence:
                # Phase 0: a bad baseline makes a live replica stale.  It only
                # becomes Synchronizing after the reliable resync is accepted.
                self.status = "Stale"
                return False, [], []
            replica = copy.deepcopy(self.baseline_records)
            for record in fields["records"]:
                replica[record["recordName"]] = record["fields"]
            self.records = replica
            self.delta_sequence = fields["delta_sequence"]
            self.last_state_us = at_us
            self.status = "Live"
            return True, [], []
        return False, [], []

    def stale_if_needed(self, at_us: int, stale_us: int) -> None:
        if self.last_state_us is not None and at_us - self.last_state_us > stale_us:
            self.status = "Stale"

    def transcript(self, at_us: int) -> str:
        session = self.records.get("SESSION_STATE", {})
        mission = self.records.get("MISSION_STATE", {})
        player_id = session.get("observed_player_entity_id", "none")
        flight = self.records.get("FLIGHT_STATE", {})
        sample = int(flight.get("producer_sample_time_us", session.get("producer_sample_time_us", "0")))
        age = max(0, at_us - sample) if sample else 0
        result = {
            "age_us": age, "baseline": self.baseline, "delta_sequence": self.delta_sequence,
            "mission_generation": mission.get("mission_generation", 0),
            "player": player_id, "pose": {"orientation": flight.get("orientation_local_to_world", []),
            "position": flight.get("position_world", [])}, "session": str(self.session_id),
            "status": self.status, "time_us": str(sample),
            "angular_velocity": flight.get("rotational_velocity_local", []),
            "velocity": flight.get("velocity_world", []),
        }
        return json.dumps(result, sort_keys=True, separators=(",", ":"))


class ConsoleClient:
    def __init__(self, sender: socket.socket | None, stale_us: int) -> None:
        self.sender, self.stale_us = sender, stale_us
        self.state = ConsoleState()
        self.fragments: dict[tuple[int, int], FragmentSet] = {}
        self.sequence = 1
        self.request_id = 1
        # (session_id, message_id, message_crc32, expiry_us), never a state
        # cache.  It permits only idempotent reliable terminal acknowledgement.
        self.session_end_tombstone: tuple[int, int, int, int] | None = None
        self.terminal_published = False
        self.hello_message_id = 0
        self.hello_payload_bytes = b""
        self.hello_first_us = 0
        self.hello_next_us = 0
        self.hello_attempt = 0
        self.pending_resync: PendingReliable | None = None

    def _send(self, data: bytes) -> None:
        if self.sender is not None:
            self.sender.send(data)

    def begin(self) -> None:
        if self.sender is None:
            return
        sent = now_us()
        nonce = secrets.randbits(64) or 1
        self.hello_message_id = self.sequence
        self.hello_payload_bytes = hello_payload(nonce, sent)
        self.hello_first_us = sent
        self.hello_attempt = 0
        self.state.hello_sent = True
        self.state.hello_nonce = nonce
        self.state.hello_t0_us = sent
        self._send_hello(sent, False)

    @staticmethod
    def _reliable_delay_us(message_id: int, attempt: int) -> int:
        # Deterministic ±10% jitter, independent from peer-controlled bytes.
        base = min(DEFAULT_RTO_US * (1 << min(attempt, 2)), MAX_RTO_US)
        jitter = 9000 + ((message_id * 1103515245 + attempt * 12345) % 2001)
        return base * jitter // 10_000

    def _send_hello(self, sent: int, retransmission: bool) -> None:
        # HELLO is retransmitted by the client-side negotiation timer, not ACKed.
        # RETRANSMISSION remains legal on retries without ACK_REQUIRED.
        flags = RETRANSMISSION if retransmission else 0
        packet = pack_header(message_type=2, flags=flags, session_id=0,
                             sequence=self.sequence, sent_us=sent, message_id=self.hello_message_id,
                             payload=self.hello_payload_bytes)
        self.sequence += 1
        self._send(packet)
        self.hello_next_us = sent + self._reliable_delay_us(self.hello_message_id, self.hello_attempt)
        self.hello_attempt += 1

    def _ack(self, header: dict[str, int], ack_flags: int) -> None:
        if self.sender is None:
            return
        sent = now_us()
        packet = pack_header(message_type=10, flags=0, session_id=header["session_id"],
                             sequence=self.sequence, sent_us=sent, message_id=self.sequence,
                             payload=ack_payload(header, ack_flags), minor=header["version_minor"])
        self.sequence += 1
        self._send(packet)

    def _respond_heartbeat(self, header: dict[str, int], fields: dict[str, Any], at_us: int) -> None:
        if (not self.state.session_begun or header["session_id"] != self.state.session_id or
                fields["kind"] != 1 or fields["probe_id"] == 0 or
                fields["receive_t1_us"] != "0" or fields["transmit_t2_us"] != "0"):
            raise ValueError("invalid HEARTBEAT request")
        receive_t1_us = at_us
        transmit_t2_us = now_us()
        payload = heartbeat_payload(fields["probe_id"], 2, int(fields["origin_t0_us"]),
                                    receive_t1_us, transmit_t2_us)
        packet = pack_header(message_type=9, flags=0, session_id=self.state.session_id,
                             sequence=self.sequence, sent_us=transmit_t2_us,
                             message_id=self.sequence, payload=payload)
        self.sequence += 1
        self._send(packet)

    def _resync(self, at_us: int) -> None:
        if self.sender is None or not self.state.session_id:
            return
        if self.pending_resync is not None:
            return
        sent = now_us()
        payload = resync_payload(self.request_id, self.state.baseline, self.state.delta_sequence, sent)
        pending = PendingReliable(12, self.sequence, self.state.session_id, payload, 1,
                                  reference.crc32_iso_hdlc(payload), at_us, at_us)
        self.pending_resync = pending
        self.sequence += 1
        self.request_id += 1
        self._send_pending_resync(at_us, False)

    def _send_pending_resync(self, at_us: int, retransmission: bool) -> None:
        pending = self.pending_resync
        if pending is None:
            return
        sent = now_us()
        flags = ACK_REQUIRED | (RETRANSMISSION if retransmission else 0)
        packet = pack_header(message_type=12, flags=flags, session_id=pending.session_id,
                             sequence=self.sequence, sent_us=sent, message_id=pending.message_id,
                             payload=pending.payload)
        self.sequence += 1
        self._send(packet)
        pending.next_us = at_us + self._reliable_delay_us(pending.message_id, pending.attempt)
        pending.attempt += 1

    def poll_reliable(self, at_us: int) -> None:
        if self.state.hello_sent and not self.state.welcomed and self.hello_message_id:
            if at_us - self.hello_first_us >= RELIABLE_WINDOW_US:
                self.state.status = "Disconnected"
            elif at_us >= self.hello_next_us:
                self._send_hello(at_us, True)
        pending = self.pending_resync
        if pending is not None:
            if at_us - pending.first_us >= RELIABLE_WINDOW_US:
                self.pending_resync = None
            elif at_us >= pending.next_us:
                self._send_pending_resync(at_us, True)

    def _on_ack(self, header: dict[str, int], fields: dict[str, Any]) -> bool:
        pending = self.pending_resync
        if (pending is None or header["session_id"] != pending.session_id or
                fields["target_message_type"] != pending.message_type or
                fields["target_message_id"] != pending.message_id or
                fields["target_fragment_count"] != pending.fragment_count or
                fields["target_message_crc32"] != f"0x{pending.message_crc32:08x}"):
            return False
        if fields["ack_flags"] & ACK_VALIDATED:
            self.pending_resync = None
            if self.state.status == "Stale":
                self.state.status = "Synchronizing"
            return True
        return False

    def receive(self, datagram: bytes, at_us: int) -> bool:
        if len(datagram) > MAX_DATAGRAM:
            raise ValueError("oversized datagram")
        header = reference.read_header(datagram)
        # ``decode_transport_sequence`` validates complete sequences.  Validate
        # the per-fragment subset here, then pass the reassembled payload to the
        # same independent logical decoder below.
        if header["magic"] != MAGIC or header["version_major"] != 1 or header["version_minor"] != 1:
            raise ValueError("unsupported FSTL header")
        if header["header_size"] != HEADER_SIZE or header["flags"] & 0xE0:
            raise ValueError("invalid FSTL header")
        if header["payload_size"] > reference.MAX_FRAGMENT_PAYLOAD or len(datagram) != HEADER_SIZE + header["payload_size"]:
            raise ValueError("invalid datagram length")
        if not 0 < header["fragment_count"] <= MAX_FRAGMENTS or header["fragment_index"] >= header["fragment_count"]:
            raise ValueError("invalid fragment index")
        expected_offset = header["fragment_index"] * reference.MAX_FRAGMENT_PAYLOAD
        expected_size = min(reference.MAX_FRAGMENT_PAYLOAD, header["message_size"] - expected_offset)
        if expected_offset > header["message_size"] or header["fragment_offset"] != expected_offset or header["payload_size"] != expected_size:
            raise ValueError("non-canonical fragment layout")
        sealed = datagram[:64] + bytes(4) + datagram[68:]
        if reference.crc32_iso_hdlc(sealed) != header["crc32"]:
            raise ValueError("datagram CRC")
        if header["message_size"] > MAX_STATE_MESSAGE or header["fragment_count"] > MAX_FRAGMENTS:
            raise ValueError("state resource limit")
        key = (header["session_id"], header["message_id"])
        group = self.fragments.setdefault(key, FragmentSet(header))
        identity_fields = ("message_type", "flags", "session_id", "frame_id", "mission_time_us", "message_id", "fragment_count", "message_size", "message_crc32")
        if any(group.header[field] != header[field] for field in identity_fields):
            raise ValueError("inconsistent fragment identity")
        group.pieces[header["fragment_index"]] = datagram[HEADER_SIZE:]
        if len(self.fragments) > 4:
            raise ValueError("reassembly quota")
        if len(group.pieces) != header["fragment_count"]:
            return False
        parts = [group.pieces[index] for index in range(header["fragment_count"])]
        payload = b"".join(parts)
        if reference.crc32_iso_hdlc(payload) != header["message_crc32"]:
            raise ValueError("message CRC")
        self.fragments.pop(key, None)
        decoded = reference.decode_message(header["message_type"], header["flags"], payload,
                                           {"senderRole": "producer", "allowedSenderRoles": ["producer"]})
        if header["message_type"] == 10:
            if self.state.session_id and header["session_id"] != self.state.session_id:
                raise ValueError("ACK outside active session")
            self._on_ack(header, decoded["fields"])
            return False
        if header["message_type"] == 9:
            self._respond_heartbeat(header, decoded["fields"], at_us)
            return False
        if header["message_type"] == 13 and self.session_end_tombstone is not None:
            session_id, message_id, message_crc32, expiry_us = self.session_end_tombstone
            if at_us > expiry_us:
                self.session_end_tombstone = None
            elif (header["session_id"], header["message_id"], header["message_crc32"]) == (session_id, message_id, message_crc32):
                if header["flags"] & ACK_REQUIRED:
                    self._ack(header, ACK_APPLIED)
                return False
        changed, validated_headers, applied_headers = self.state.apply(
            header["message_type"], decoded["fields"], payload, header, at_us)
        for ack_header in validated_headers:
            if ack_header["flags"] & ACK_REQUIRED:
                self._ack(ack_header, ACK_VALIDATED)
        for ack_header in applied_headers:
            if ack_header["flags"] & ACK_REQUIRED:
                self._ack(ack_header, ACK_APPLIED)
        if header["message_type"] == 13 and changed:
            # No fragment/reassembly survives a validated terminal publication.
            self.fragments.clear()
            self.session_end_tombstone = (header["session_id"], header["message_id"],
                                          header["message_crc32"], at_us + SESSION_END_TOMBSTONE_US)
            self.terminal_published = True
        if header["message_type"] == 6 and changed:
            # A fresh committed snapshot is also a successful resynchronizing
            # lifecycle response; release any retained outbound request.
            self.pending_resync = None
        if header["message_type"] == 7 and not changed:
            self._resync(at_us)
        return changed


def replay(paths: list[Path], stale_us: int) -> int:
    client = ConsoleClient(None, stale_us)
    # A capture contains inbound datagrams only.  Reconstruct the exact local
    # HELLO identity from its recorded matching WELCOME before replaying it, so
    # the normal handshake follows the same nonce/t0 correlation as live UDP.
    for path in paths:
        packet = path.read_bytes()
        header = reference.read_header(packet)
        if header["message_type"] != 3 or header["fragment_count"] != 1:
            continue
        payload = packet[HEADER_SIZE:]
        welcome = reference.decode_message(3, header["flags"], payload,
                                           {"senderRole": "producer", "allowedSenderRoles": ["producer"]})["fields"]
        nonce, t0 = int(welcome["client_nonce"]), int(welcome["client_send_t0_us"])
        client.state.hello_sent = True
        client.state.hello_nonce = nonce
        client.state.hello_t0_us = t0
        client.hello_message_id = 1
        client.hello_payload_bytes = hello_payload(nonce, t0)
        client.hello_first_us = t0
        break
    for path in paths:
        packet = path.read_bytes()
        header = reference.read_header(packet)
        tick = header["sent_time_us"]
        changed = client.receive(packet, tick)
        # A rejected delta deliberately does not publish data, but its required
        # resync transition is observable proof-client state and belongs in the
        # deterministic transcript.
        if changed or (header["message_type"] == 7 and client.state.status in ("Stale", "Synchronizing")):
            print(client.state.transcript(tick))
    if not client.terminal_published:
        client.state.stale_if_needed((client.state.last_state_us or 0) + stale_us + 1, stale_us)
        print(client.state.transcript((client.state.last_state_us or 0) + stale_us + 1))
    return 0


def live(host: str, port: int, seconds: float, stale_us: int) -> int:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.1)
        sock.connect((host, port))
        client = ConsoleClient(sock, stale_us)
        client.begin()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            client.poll_reliable(now_us())
            try:
                data = sock.recv(MAX_DATAGRAM)
                if client.receive(data, now_us()):
                    print(client.state.transcript(now_us()))
            except socket.timeout:
                pass
            client.state.stale_if_needed(now_us(), stale_us)
        if not client.terminal_published:
            print(client.state.transcript(now_us()))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="independent FSTL 1.1 proof console")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=42042)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--stale-ms", type=int, default=3000)
    parser.add_argument("--replay", type=Path, nargs="+", help="raw datagrams for deterministic transcript")
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535 or args.seconds <= 0 or args.stale_ms < 1:
        parser.error("invalid port, duration or stale timeout")
    try:
        return replay(args.replay, args.stale_ms * 1000) if args.replay else live(args.host, args.port, args.seconds, args.stale_ms * 1000)
    except (OSError, ValueError, reference.DecodeFailure) as exc:
        print(f"console error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

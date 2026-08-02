#!/usr/bin/env python3
"""External two-client observer for the Phase 2 Release scenario.

The normal console remains the neutral single-client observer.  This tool
imports that independent decoder and adds only a bounded ACK scheduling policy:
the fast peer behaves normally and drops one replaceable delta, while the slow
peer withholds APPLIED for the first snapshot that depends on a new manifest.
It never imports or controls the C++ producer.
"""

from __future__ import annotations

import argparse
import json
import select
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

_TOOLS_DIR = str(Path(__file__).resolve().parent)
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

import fstl_console_client as console
import fstl_reference_decoder as reference


EventSink = Callable[[dict[str, Any]], None]
MAX_HELD_APPLIED_ACKS = 64
EVIDENCE_RECORD_NAMES = frozenset({
    "ENTITY_LIFECYCLE",
    "SHIP_IDENTITY",
    "CARGO_SCAN_STATE",
    "DOCKING_STATE",
    "SUPPORT_STATE",
})


@dataclass
class HeldAck:
    header: dict[str, int]
    ack_flags: int


class AppliedSnapshotHold:
    """Hold one logical post-manifest snapshot APPLIED transaction."""

    def __init__(self, hold_us: int, emit: EventSink, client_name: str) -> None:
        if hold_us < 1:
            raise ValueError("hold duration must be positive")
        self.hold_us = hold_us
        self.emit = emit
        self.client_name = client_name
        self.initial_manifest_id: int | None = None
        self.held_manifest_id: int | None = None
        self.held_snapshot_id: int | None = None
        self.release_at_us: int | None = None
        self.hold_started = False
        self.hold_active = False
        self.held: dict[tuple[int, int, int], HeldAck] = {}
        self.manifests_during_hold: list[int] = []

    def _event(self, event: dict[str, Any], at_us: int) -> None:
        _, observed_at_utc = console.local_observation()
        event["observedAtMonotonicUs"] = at_us
        event["observedAtUtc"] = observed_at_utc
        self.emit(event)

    @staticmethod
    def _identity(header: dict[str, int]) -> tuple[int, int, int]:
        return (
            header["message_id"],
            header["message_crc32"],
            header["fragment_count"],
        )

    def route(self, header: dict[str, int], ack_flags: int, manifest_id: int,
              snapshot_id: int, at_us: int) -> bool:
        if header["message_type"] != 6 or ack_flags != console.ACK_APPLIED:
            return False
        if self.initial_manifest_id is None:
            self.initial_manifest_id = manifest_id
            self._event({
                "kind": "scenario",
                "client": self.client_name,
                "event": "initial-snapshot-applied",
                "manifestId": manifest_id,
                "snapshotId": snapshot_id,
            }, at_us)
            return False
        if self.hold_active and manifest_id == self.held_manifest_id:
            if snapshot_id != self.held_snapshot_id:
                raise ValueError("slow APPLIED hold received a second snapshot transaction")
            identity = self._identity(header)
            if identity not in self.held and len(self.held) >= MAX_HELD_APPLIED_ACKS:
                raise ValueError("slow APPLIED hold quota")
            self.held.setdefault(identity, HeldAck(dict(header), ack_flags))
            return True
        if self.hold_started or manifest_id == self.initial_manifest_id:
            return False

        self.hold_started = True
        self.hold_active = True
        self.held_manifest_id = manifest_id
        self.held_snapshot_id = snapshot_id
        self.release_at_us = at_us + self.hold_us
        self.held[self._identity(header)] = HeldAck(dict(header), ack_flags)
        self._event({
            "kind": "scenario",
            "client": self.client_name,
            "event": "snapshot-applied-held",
            "manifestId": manifest_id,
            "snapshotId": snapshot_id,
            "holdUs": self.hold_us,
            "heldAckCount": len(self.held),
        }, at_us)
        return True

    def observe_manifest(self, manifest_id: int, at_us: int) -> None:
        if not self.hold_active or manifest_id == self.held_manifest_id:
            return
        self.manifests_during_hold.append(manifest_id)
        self._event({
            "kind": "scenario",
            "client": self.client_name,
            "event": "manifest-arrived-during-hold",
            "manifestId": manifest_id,
            "heldManifestId": self.held_manifest_id,
        }, at_us)

    def release_due(self, at_us: int,
                    send_ack: Callable[[dict[str, int], int], None]) -> bool:
        if not self.hold_active or self.release_at_us is None or at_us < self.release_at_us:
            return False
        held = list(self.held.values())
        for item in held:
            send_ack(item.header, item.ack_flags)
        self.held.clear()
        self.hold_active = False
        self._event({
            "kind": "scenario",
            "client": self.client_name,
            "event": "snapshot-applied-released",
            "manifestId": self.held_manifest_id,
            "snapshotId": self.held_snapshot_id,
            "releasedAckCount": len(held),
            "manifestArrivalsDuringHold": list(self.manifests_during_hold),
        }, at_us)
        return True


class ScenarioConsoleClient(console.ConsoleClient):
    """Console client with observable role and optional slow-APPLIED policy."""

    def __init__(self, sender: socket.socket, stale_us: int, name: str,
                 emit: EventSink, *, drop_once_delta: bool = False,
                 applied_hold_us: int | None = None) -> None:
        super().__init__(sender, stale_us, drop_once_delta)
        self.name = name
        self.emit = emit
        self.manifest_ids: list[int] = []
        self.snapshot_ids: list[int] = []
        self.ack_counts = {"validated": 0, "applied": 0, "held": 0, "released": 0}
        self.delta_applied_count = 0
        self.delta_dropped_count = 0
        self.delta_rejected_count = 0
        self.delta_baselines: set[int] = set()
        self._delta_headers: dict[tuple[int, int], tuple[int, int]] = {}
        self._receive_at_us: int | None = None
        self._receive_at_utc: str | None = None
        self.hold = (
            AppliedSnapshotHold(applied_hold_us, emit, name)
            if applied_hold_us is not None
            else None
        )

    def _emit_at(self, event: dict[str, Any], at_us: int,
                 at_utc: str | None = None) -> None:
        if at_utc is None:
            _, at_utc = console.local_observation()
        event["observedAtMonotonicUs"] = at_us
        event["observedAtUtc"] = at_utc
        self.emit(event)

    def _ack_event(self, event_name: str, header: dict[str, int],
                   ack_flags: int, at_us: int, at_utc: str | None = None) -> None:
        self._emit_at({
            "kind": "scenario",
            "client": self.name,
            "event": event_name,
            "targetMessageType": header["message_type"],
            "targetMessageId": header["message_id"],
            "targetFragmentCount": header["fragment_count"],
            "targetMessageCrc32": f"0x{header['message_crc32']:08x}",
            "sessionId": header["session_id"],
            "ackFlags": ack_flags,
            "manifestId": self.state.manifest_id,
            "snapshotId": self.state.baseline,
        }, at_us, at_utc)

    def _ack(self, header: dict[str, int], ack_flags: int) -> None:
        at_us = self._receive_at_us or console.now_us()
        at_utc = self._receive_at_utc
        if self.hold is not None and self.hold.route(
            header,
            ack_flags,
            self.state.manifest_id,
            self.state.baseline,
            at_us,
        ):
            self.ack_counts["held"] += 1
            self._ack_event("ack-held", header, ack_flags, at_us, at_utc)
            return
        super()._ack(header, ack_flags)
        self.ack_counts["applied" if ack_flags == console.ACK_APPLIED else "validated"] += 1
        self._ack_event("ack-sent", header, ack_flags, at_us, at_utc)

    def receive(self, datagram: bytes, at_us: int, at_utc: str | None = None) -> bool:
        header = reference.read_header(datagram)
        message_key = (header["session_id"], header["message_id"])
        if (
            header["message_type"] == 7
            and header["fragment_index"] == 0
            and len(datagram) >= console.HEADER_SIZE + 8
        ):
            self._delta_headers[message_key] = (
                int.from_bytes(
                    datagram[console.HEADER_SIZE:console.HEADER_SIZE + 4], "little"
                ),
                int.from_bytes(
                    datagram[console.HEADER_SIZE + 4:console.HEADER_SIZE + 8], "little"
                ),
            )
        previous_manifest = self.state.manifest_id
        previous_snapshot = self.state.baseline
        previous_injections = len(self.state.injections)
        self._receive_at_us = at_us
        self._receive_at_utc = at_utc
        try:
            changed = super().receive(datagram, at_us, at_utc)
        finally:
            self._receive_at_us = None
            self._receive_at_utc = None
        if self.state.manifest_id != previous_manifest:
            self.manifest_ids.append(self.state.manifest_id)
            self._emit_at({
                "kind": "scenario",
                "client": self.name,
                "event": "manifest-committed",
                "manifestId": self.state.manifest_id,
            }, at_us, at_utc)
            if self.hold is not None:
                self.hold.observe_manifest(self.state.manifest_id, at_us)
        if self.state.baseline != previous_snapshot:
            self.snapshot_ids.append(self.state.baseline)
            self._emit_at({
                "kind": "scenario",
                "client": self.name,
                "event": "snapshot-committed",
                "manifestId": self.state.manifest_id,
                "snapshotId": self.state.baseline,
            }, at_us, at_utc)
        message_complete = message_key not in self.fragments
        if header["message_type"] == 7 and message_complete:
            received_baseline, received_sequence = self._delta_headers.pop(
                message_key, (None, None)
            )
            delta_event = {
                "kind": "scenario",
                "client": self.name,
                "targetMessageId": header["message_id"],
                "receivedBaselineSnapshotId": received_baseline,
                "receivedDeltaSequence": received_sequence,
                "resultingBaselineSnapshotId": self.state.baseline,
                "resultingDeltaSequence": self.state.delta_sequence,
                "manifestId": self.state.manifest_id,
            }
            if received_baseline is not None:
                self.delta_baselines.add(received_baseline)
            if len(self.state.injections) != previous_injections:
                self.delta_dropped_count += 1
                delta_event["event"] = "delta-dropped"
            elif changed:
                self.delta_applied_count += 1
                delta_event["event"] = "delta-applied"
            else:
                self.delta_rejected_count += 1
                delta_event["event"] = "delta-rejected"
                delta_event["status"] = self.state.status
            self._emit_at(delta_event, at_us, at_utc)
        return changed

    def _send_released_ack(self, header: dict[str, int], ack_flags: int) -> None:
        at_us, at_utc = console.local_observation()
        super()._ack(header, ack_flags)
        self.ack_counts["released"] += 1
        self._ack_event("ack-released", header, ack_flags, at_us, at_utc)

    def poll_scenario(self, at_us: int) -> None:
        self.poll_reliable(at_us)
        if self.hold is not None:
            self.hold.release_due(at_us, self._send_released_ack)

    def facts(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "sessionId": self.state.session_id,
            "manifestIds": list(self.manifest_ids),
            "snapshotIds": list(self.snapshot_ids),
            "finalManifestId": self.state.manifest_id,
            "finalSnapshotId": self.state.baseline,
            "finalStatus": self.state.status,
            "finalDeltaSequence": self.state.delta_sequence,
            "injections": list(self.state.injections),
            "ackCounts": dict(self.ack_counts),
            "deltaAppliedCount": self.delta_applied_count,
            "deltaDroppedCount": self.delta_dropped_count,
            "deltaRejectedCount": self.delta_rejected_count,
            "deltaBaselines": sorted(self.delta_baselines),
            "evidenceRecords": _evidence_records(self),
        }
        if self.hold is not None:
            result["slowApplied"] = {
                "initialManifestId": self.hold.initial_manifest_id,
                "heldManifestId": self.hold.held_manifest_id,
                "heldSnapshotId": self.hold.held_snapshot_id,
                "holdStarted": self.hold.hold_started,
                "holdActiveAtEnd": self.hold.hold_active,
                "manifestArrivalsDuringHold": list(self.hold.manifests_during_hold),
            }
        return result


def _emit(event: dict[str, Any]) -> None:
    print(json.dumps(event, sort_keys=True, separators=(",", ":")), flush=True)


def _is_synchronized(client: ScenarioConsoleClient) -> bool:
    state = client.state
    return (
        state.status == "Live"
        and state.manifest_applied
        and state.keyframe_applied
        and not state.reliable_dependency_pending
        and not state.transactions
        and not state.manifest_transactions
        and state.required_manifest_id in (0, state.manifest_id)
        and state.baseline != 0
    )


def _evidence_records(client: ScenarioConsoleClient) -> list[dict[str, Any]]:
    records = (
        record
        for record in client.state.record_instances.values()
        if record["recordName"] in EVIDENCE_RECORD_NAMES
    )
    return sorted(
        (
            {
                "identity": console._record_identity(record),
                "recordName": record["recordName"],
                "fields": record["fields"],
            }
            for record in records
        ),
        key=lambda record: record["identity"],
    )


def _compact_state_event(
    client: ScenarioConsoleClient, at_us: int, at_utc: str
) -> dict[str, Any]:
    state = client.state
    session = state.records.get("SESSION_STATE", {})
    mission = state.records.get("MISSION_STATE", {})
    return {
        "kind": "state",
        "client": client.name,
        "observedAtMonotonicUs": at_us,
        "observedAtUtc": at_utc,
        "status": state.status,
        "sessionId": state.session_id,
        "missionGeneration": mission.get("mission_generation", 0),
        "manifestId": state.manifest_id,
        "snapshotId": state.baseline,
        "deltaSequence": state.delta_sequence,
        "playerEntityId": session.get("observed_player_entity_id", 0),
        "recordCount": len(state.record_instances),
        "synchronized": _is_synchronized(client),
    }


def _state_event(client: ScenarioConsoleClient, at_us: int, at_utc: str) -> None:
    _emit(_compact_state_event(client, at_us, at_utc))


def _observe_staleness(client: ScenarioConsoleClient, at_us: int, at_utc: str,
                       stale_us: int) -> bool:
    previous_status = client.state.status
    client.state.stale_if_needed(at_us, at_utc, stale_us)
    if client.state.status == previous_status:
        return False
    client._emit_at({
        "kind": "scenario",
        "client": client.name,
        "event": "status-transition",
        "previousStatus": previous_status,
        "status": client.state.status,
        "reason": client.state.stale_reason,
    }, at_us, at_utc)
    _state_event(client, at_us, at_utc)
    return True


def _connected_socket(host: str, port: int) -> socket.socket:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.setblocking(False)
    sock.connect((host, port))
    return sock


def _drain_socket(
    sock: socket.socket, client: ScenarioConsoleClient
) -> int:
    received = 0
    while True:
        try:
            data = sock.recv(console.MAX_DATAGRAM)
        except BlockingIOError:
            return received
        received_at_us, received_at_utc = console.local_observation()
        received += 1
        if client.receive(data, received_at_us, received_at_utc):
            observed_at_us, observed_at_utc = console.local_observation()
            _state_event(client, observed_at_us, observed_at_utc)


def run(host: str, port: int, seconds: float, stale_us: int,
        hold_us: int) -> int:
    sockets = [_connected_socket(host, port), _connected_socket(host, port)]
    try:
        fast = ScenarioConsoleClient(
            sockets[0], stale_us, "fast", _emit, drop_once_delta=True
        )
        slow = ScenarioConsoleClient(
            sockets[1], stale_us, "slow", _emit, applied_hold_us=hold_us
        )
        clients = {sockets[0]: fast, sockets[1]: slow}
        for client in clients.values():
            client.begin()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            at_us = console.now_us()
            for client in clients.values():
                client.poll_scenario(at_us)
            readable, _, _ = select.select(list(clients), [], [], 0.1)
            for sock in readable:
                _drain_socket(sock, clients[sock])
            observed_at_us, observed_at_utc = console.local_observation()
            for client in clients.values():
                _observe_staleness(
                    client, observed_at_us, observed_at_utc, stale_us
                )

        observed_at_us, observed_at_utc = console.local_observation()
        for client in clients.values():
            if not client.terminal_published:
                _state_event(client, observed_at_us, observed_at_utc)
        _emit({
            "kind": "scenario-summary",
            "durationSeconds": seconds,
            "fast": fast.facts(),
            "slow": slow.facts(),
        })
        return 0
    finally:
        for sock in sockets:
            sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="independent two-client FSTL 1.1 Phase 2 observation scenario"
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=42042)
    parser.add_argument("--seconds", type=float, default=120.0)
    parser.add_argument("--stale-ms", type=int, default=3000)
    parser.add_argument("--slow-hold-ms", type=int, default=8_000)
    args = parser.parse_args()
    if (
        args.port < 1
        or args.port > 65535
        or args.seconds <= 0
        or args.stale_ms < 1
        or args.slow_hold_ms < 1
    ):
        parser.error("invalid port, duration, stale timeout or slow hold")
    try:
        return run(
            args.host,
            args.port,
            args.seconds,
            args.stale_ms * 1000,
            args.slow_hold_ms * 1000,
        )
    except (OSError, ValueError, reference.DecodeFailure) as exc:
        print(f"scenario error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

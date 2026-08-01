#!/usr/bin/env python3
"""Small independent FSTL 1.1 observation client.

It deliberately imports only :mod:`fstl_reference_decoder`; it never imports
the C++ producer, its DTOs, fixture generators, or a generated layout. The
``--replay`` mode produces deterministic transcripts in CI while the default
mode is a bounded UDP observation client for a configured producer.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
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


def utc_iso8601_from_ns(timestamp_ns: int) -> str:
    """Format an absolute client-observation time without losing microseconds."""
    seconds, nanoseconds = divmod(timestamp_ns, 1_000_000_000)
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(seconds)) + f".{nanoseconds // 1000:06d}Z"


def local_observation() -> tuple[int, str]:
    """Return the client's monotonic and UTC clocks at one observation point."""
    return now_us(), utc_iso8601_from_ns(time.time_ns())


def replay_observation(at_us: int) -> tuple[int, str]:
    """Return deterministic simulated client clocks for an offline replay."""
    return at_us, utc_iso8601_from_ns(at_us * 1000)


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


def _record_identity(record: dict[str, Any]) -> str:
    """Return the stable public identity of one decoded state atom."""
    fields = record["fields"]
    parts = [record["recordName"]]
    for name in ("manifest_generation", "class_id", "weapon_class_id", "entity_id",
                 "subsystem_id", "event_id"):
        if name in fields:
            parts.append(f"{name}={fields[name]}")
    return "/".join(parts)


def _as_float(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _trunc_signed(value: int, divisor: int) -> int:
    """Signed integer division truncated toward zero without float conversion."""
    return value // divisor if value >= 0 else -((-value) // divisor)


def _ratio(current: Any, maximum: Any) -> dict[str, Any]:
    numerator, denominator = _as_float(current), _as_float(maximum)
    if numerator is None or denominator is None or denominator <= 0.0:
        return {"available": False, "reason": "missing-or-nonpositive-denominator", "value": None}
    return {"available": True, "reason": None, "value": min(1.0, max(0.0, numerator / denominator))}


def _rotate_world_to_local(orientation: list[Any], velocity: list[Any]) -> list[float] | None:
    if len(orientation) != 4 or len(velocity) != 3:
        return None
    values = [_as_float(value) for value in (*orientation, *velocity)]
    if any(value is None for value in values):
        return None
    w, x, y, z, vx, vy, vz = (float(value) for value in values)
    # q^-1 * [0,v] * q, with FSTL quaternion order [w,x,y,z].
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return [
        vx - w * tx + (y * tz - z * ty),
        vy - w * ty + (z * tx - x * tz),
        vz - w * tz + (x * ty - y * tx),
    ]


class DashboardProjection:
    """Independent Phase 2 proof projection and machine-readable A/C/D inventory."""

    LIFECYCLE_LABELS = {
        0: "SPAWNING", 1: "ACTIVE", 2: "DEPARTING",
        3: "DYING", 4: "DESTROYED", 5: "REMOVED",
    }
    FORMULA_CATALOG = {
        "sqrt(vx^2+vy^2+vz^2)": (
            "p2.dashboard.speed.v1", ["wire:FLIGHT_STATE.velocity_world"]),
        "conjugate(orientation_local_to_world) * velocity_world": (
            "p2.dashboard.velocity-local.v1",
            ["wire:FLIGHT_STATE.orientation_local_to_world", "wire:FLIGHT_STATE.velocity_world"]),
        "closed lifecycle_phase label mapping": (
            "p2.dashboard.lifecycle-label.v1", ["wire:ENTITY_LIFECYCLE.lifecycle_phase"]),
        "clamp(hull_strength/dynamic_max_hull,0,1)": (
            "p2.dashboard.hull-ratio.v1",
            ["wire:DAMAGE_STATE.hull_strength", "wire:DAMAGE_STATE.dynamic_max_hull"]),
        "sum(segment_current_hits)": (
            "p2.dashboard.shield-current-total.v1", ["wire:SHIELD_STATE.segment_current_hits"]),
        "sum(segment_max_hits)": (
            "p2.dashboard.shield-max-total.v1", ["wire:SHIELD_STATE.segment_max_hits"]),
        "clamp(sum(segment_current_hits)/sum(segment_max_hits),0,1)": (
            "p2.dashboard.shield-ratio.v1",
            ["wire:SHIELD_STATE.has_shields", "wire:SHIELD_STATE.segment_current_hits",
             "wire:SHIELD_STATE.segment_max_hits"]),
        "clamp(current_hits/max_hits,0,1)": (
            "p2.dashboard.subsystem-integrity-ratio.v1",
            ["wire:SUBSYSTEM_STATE.current_hits", "wire:SUBSYSTEM_STATE.max_hits"]),
        "max_hits>0 && current_hits<=0": (
            "p2.dashboard.subsystem-destroyed.v1",
            ["wire:SUBSYSTEM_STATE.current_hits", "wire:SUBSYSTEM_STATE.max_hits"]),
        "unavailable: Phase 2 wire lacks exact ets_properties applicability": (
            "p2.dashboard.ets-share-unavailable.v1",
            ["wire:ENERGY_STATE.ets_mode", "wire:ENERGY_STATE.ets_shields_index",
             "wire:ENERGY_STATE.ets_weapons_index", "wire:ENERGY_STATE.ets_engines_index"]),
        "clamp(weapon_energy_current/weapon_energy_max,0,1)": (
            "p2.dashboard.weapon-energy-ratio.v1",
            ["wire:ENERGY_STATE.weapon_energy_current", "wire:ENERGY_STATE.weapon_energy_max"]),
        "clamp(afterburner_fuel_current/afterburner_fuel_max,0,1)": (
            "p2.dashboard.fuel-ratio.v1",
            ["wire:ENERGY_STATE.afterburner_fuel_current",
             "wire:ENERGY_STATE.afterburner_fuel_max"]),
        "clamp(ammo_current/ammo_initial,0,1)": (
            "p2.dashboard.ammo-ratio.v1",
            ["wire:WEAPON_STATE.ammo_current", "wire:WEAPON_STATE.ammo_initial"]),
        "clamp(quantity_current/quantity_max,0,1)": (
            "p2.dashboard.countermeasure-ratio.v1",
            ["wire:WEAPON_STATE.countermeasure.quantity_current",
             "wire:WEAPON_STATE.countermeasure.quantity_max"]),
        "client_monotonic_time_us+smoothed_offset-producer_sample_time_us": (
            "p2.dashboard.sample-age.v1",
            ["client:monotonic_time_us", "client:smoothed_offset_us",
             "wire:producer_sample_time_us"]),
        "age_us > 3*block_period_us+100000": (
            "p2.dashboard.stale.v1",
            ["derived:p2.dashboard.sample-age.v1", "config:flightHz", "config:systemsHz",
             "config:missionHeartbeatMs"]),
        "unavailable without a documented projection input": (
            "p2.dashboard.hud-coordinates-unavailable.v1",
            ["contract:P2-REQ-032", "wire:projection-input-absent"]),
        "wire duration or explicitly observed stable rate only": (
            "p2.dashboard.support-eta-unavailable.v1",
            ["wire:SUPPORT_STATE", "wire:stable-rate-or-duration-absent"]),
        "manifest/keyframe APPLIED, no pending candidate, known baseline": (
            "p2.dashboard.synchronized.v1",
            ["client:manifest_applied", "client:keyframe_applied",
             "client:pending_transactions", "client:baseline"]),
    }

    def __init__(self, state: "ConsoleState", at_us: int,
                 smoothed_offset_us: int | None = None,
                 offset_filter_valid: bool | None = None,
                 flight_hz: int = 30, systems_hz: int = 10,
                 mission_heartbeat_ms: int = 500) -> None:
        self.state = state
        self.at_us = at_us
        self.smoothed_offset_us = smoothed_offset_us
        self.offset_filter_valid = (
            smoothed_offset_us is not None
            if offset_filter_valid is None
            else offset_filter_valid
        )
        self.flight_hz = flight_hz
        self.systems_hz = systems_hz
        self.mission_heartbeat_ms = mission_heartbeat_ms
        self.inventory: list[dict[str, Any]] = []
        self.derived: dict[str, Any] = {}

    def _raw_leaf(self, path: str, value: Any, source: str) -> None:
        if isinstance(value, dict):
            for name in sorted(value):
                self._raw_leaf(f"{path}.{name}", value[name], source)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                self._raw_leaf(f"{path}[{index}]", item, source)
        else:
            self.inventory.append({
                "available": True,
                "kind": "A",
                "path": path,
                "source": source,
                "value": value,
            })

    def _add_derived(self, path: str, formula: str, result: dict[str, Any]) -> None:
        catalog = self.FORMULA_CATALOG.get(formula)
        if catalog is None:
            raise ValueError(f"uncatalogued dashboard formula: {formula}")
        formula_id, provenance = catalog
        item = {
            "kind": "D",
            "path": path,
            "formula": formula,
            "formulaId": formula_id,
            "provenance": provenance,
            "source": f"formula:{formula_id}",
            **result,
        }
        self.inventory.append(item)
        self.derived[path] = {
            "available": item["available"],
            "reason": item.get("reason"),
            "value": item.get("value"),
        }

    def _records(self, name: str) -> list[dict[str, Any]]:
        return [
            record["fields"] for _, record in sorted(self.state.record_instances.items())
            if record["recordName"] == name
        ]

    def _per_entity(self, name: str) -> dict[str, dict[str, Any]]:
        return {str(record["entity_id"]): record for record in self._records(name)
                if "entity_id" in record}

    def build(self) -> dict[str, Any]:
        for identity, record in sorted(self.state.record_instances.items()):
            self._raw_leaf(f"records.{identity}", record["fields"],
                           f"wire:{record['recordName']}:v{record['recordVersion']}")
        for identity, record in sorted(self.state.manifest_records.items()):
            self._raw_leaf(f"manifest.{identity}", record["fields"],
                           f"wire:{record['recordName']}:v{record['recordVersion']}")

        flights = self._per_entity("FLIGHT_STATE")
        for entity, flight in flights.items():
            velocity = [_as_float(value) for value in flight.get("velocity_world", [])]
            speed = None if len(velocity) != 3 or any(value is None for value in velocity) else math.sqrt(
                sum(float(value) * float(value) for value in velocity))
            self._add_derived(
                f"entities.{entity}.speed",
                "sqrt(vx^2+vy^2+vz^2)",
                {"available": speed is not None, "reason": None if speed is not None else "missing-velocity",
                 "value": speed},
            )
            local = _rotate_world_to_local(
                flight.get("orientation_local_to_world", []),
                flight.get("velocity_world", []),
            )
            self._add_derived(
                f"entities.{entity}.velocity_local",
                "conjugate(orientation_local_to_world) * velocity_world",
                {"available": local is not None,
                 "reason": None if local is not None else "missing-pose", "value": local},
            )

        for entity, lifecycle in self._per_entity("ENTITY_LIFECYCLE").items():
            phase = lifecycle.get("lifecycle_phase")
            label = self.LIFECYCLE_LABELS.get(phase)
            self._add_derived(
                f"entities.{entity}.lifecycle_label",
                "closed lifecycle_phase label mapping",
                {"available": label is not None,
                 "reason": None if label is not None else "invalid-lifecycle-phase", "value": label},
            )

        for entity, damage in self._per_entity("DAMAGE_STATE").items():
            self._add_derived(
                f"entities.{entity}.hull_ratio",
                "clamp(hull_strength/dynamic_max_hull,0,1)",
                _ratio(damage.get("hull_strength"), damage.get("dynamic_max_hull")),
            )

        for entity, shield in self._per_entity("SHIELD_STATE").items():
            current = [_as_float(value) for value in shield.get("segment_current_hits", [])]
            maximum = [_as_float(value) for value in shield.get("segment_max_hits", [])]
            valid = not any(value is None for value in (*current, *maximum))
            current_total = sum(float(value) for value in current) if valid else None
            maximum_total = sum(float(value) for value in maximum) if valid else None
            self._add_derived(
                f"entities.{entity}.shield_current_total", "sum(segment_current_hits)",
                {"available": current_total is not None, "reason": None if valid else "invalid-segments",
                 "value": current_total},
            )
            self._add_derived(
                f"entities.{entity}.shield_max_total", "sum(segment_max_hits)",
                {"available": maximum_total is not None, "reason": None if valid else "invalid-segments",
                 "value": maximum_total},
            )
            ratio = (_ratio(current_total, maximum_total)
                     if shield.get("has_shields") and valid
                     else {"available": False, "reason": "shields-absent", "value": None})
            self._add_derived(
                f"entities.{entity}.shield_ratio",
                "clamp(sum(segment_current_hits)/sum(segment_max_hits),0,1)",
                ratio,
            )

        for subsystem in self._records("SUBSYSTEM_STATE"):
            entity, subsystem_id = str(subsystem["entity_id"]), subsystem["subsystem_id"]
            prefix = f"entities.{entity}.subsystems.{subsystem_id}"
            ratio = _ratio(subsystem.get("current_hits"), subsystem.get("max_hits"))
            self._add_derived(f"{prefix}.integrity_ratio",
                              "clamp(current_hits/max_hits,0,1)", ratio)
            current, maximum = (_as_float(subsystem.get("current_hits")),
                                _as_float(subsystem.get("max_hits")))
            destroyed = maximum is not None and maximum > 0.0 and current is not None and current <= 0.0
            self._add_derived(
                f"{prefix}.destroyed", "max_hits>0 && current_hits<=0",
                {"available": current is not None and maximum is not None,
                 "reason": None if current is not None and maximum is not None else "missing-hits",
                 "value": destroyed if current is not None and maximum is not None else None},
            )

        for entity, energy in self._per_entity("ENERGY_STATE").items():
            for group in ("shields", "weapons", "engines"):
                self._add_derived(
                    f"entities.{entity}.ets_share.{group}",
                    "unavailable: Phase 2 wire lacks exact ets_properties applicability",
                    {"available": False, "reason": "ets-applicability-not-on-wire", "value": None},
                )
            for current_name, maximum_name, output in (
                ("weapon_energy_current", "weapon_energy_max", "weapon_energy_ratio"),
                ("afterburner_fuel_current", "afterburner_fuel_max", "fuel_ratio"),
            ):
                self._add_derived(
                    f"entities.{entity}.{output}",
                    f"clamp({current_name}/{maximum_name},0,1)",
                    _ratio(energy.get(current_name), energy.get(maximum_name)),
                )

        for entity, weapon in self._per_entity("WEAPON_STATE").items():
            for family in ("primary_banks", "secondary_banks"):
                for index, bank in enumerate(weapon.get(family, [])):
                    self._add_derived(
                        f"entities.{entity}.{family}[{index}].ammo_ratio",
                        "clamp(ammo_current/ammo_initial,0,1)",
                        _ratio(bank.get("ammo_current"), bank.get("ammo_initial")),
                    )
            countermeasure = weapon.get("countermeasure")
            if isinstance(countermeasure, dict):
                self._add_derived(
                    f"entities.{entity}.countermeasure.quantity_ratio",
                    "clamp(quantity_current/quantity_max,0,1)",
                    _ratio(countermeasure.get("quantity_current"), countermeasure.get("quantity_max")),
                )

        sample_periods = {
            "FLIGHT_STATE": math.ceil(1_000_000 / self.flight_hz),
            "CONTROL_STATE": math.ceil(1_000_000 / self.flight_hz),
        }
        default_system_period = math.ceil(1_000_000 / self.systems_hz)
        estimated_now = (
            self.at_us + self.smoothed_offset_us
            if self.offset_filter_valid and self.smoothed_offset_us is not None
            else None
        )
        if estimated_now is not None and not -(1 << 63) <= estimated_now < (1 << 63):
            estimated_now = None
        for identity, record in sorted(self.state.record_instances.items()):
            sample = record["fields"].get("producer_sample_time_us")
            sample_value = int(sample) if sample is not None else None
            period = sample_periods.get(
                record["recordName"],
                self.mission_heartbeat_ms * 1000
                if record["recordName"] in ("SESSION_STATE", "MISSION_STATE")
                else default_system_period,
            )
            valid_age = estimated_now is not None and sample_value is not None and estimated_now >= sample_value
            age = estimated_now - sample_value if valid_age else None
            self._add_derived(
                f"records.{identity}.age_us",
                "client_monotonic_time_us+smoothed_offset-producer_sample_time_us",
                {"available": valid_age, "reason": None if valid_age else "invalid-or-missing-clock-offset",
                 "value": age},
            )
            self._add_derived(
                f"records.{identity}.stale",
                "age_us > 3*block_period_us+100000",
                {"available": valid_age, "reason": None if valid_age else "invalid-or-missing-age",
                 "value": age > 3 * period + 100_000 if valid_age else None},
            )

        self._add_derived(
            "dashboard.hud_coordinates", "unavailable without a documented projection input",
            {"available": False, "reason": "projection-input-not-on-wire", "value": None},
        )
        self._add_derived(
            "dashboard.support_eta_us", "wire duration or explicitly observed stable rate only",
            {"available": False, "reason": "no-stable-rate-or-duration", "value": None},
        )
        synchronized = (
            self.state.status == "Live"
            and self.state.manifest_applied
            and self.state.keyframe_applied
            and not self.state.reliable_dependency_pending
            and not self.state.transactions
            and not self.state.manifest_transactions
            and self.state.required_manifest_id in (0, self.state.manifest_id)
            and self.state.baseline != 0
        )
        synchronization_conditions = {
            "lastManifestApplied": self.state.manifest_applied,
            "lastKeyframeApplied": self.state.keyframe_applied,
            "noPendingCandidateOrReliableDependency": (
                not self.state.reliable_dependency_pending
                and not self.state.transactions
                and not self.state.manifest_transactions
            ),
            "baselineKnown": self.state.baseline != 0,
        }
        self._add_derived(
            "transport.synchronized",
            "manifest/keyframe APPLIED, no pending candidate, known baseline",
            {"available": True, "reason": None, "value": synchronized},
        )
        return {
            "derived": self.derived,
            "inventory": self.inventory,
            "inventorySummary": {
                "available": sum(1 for item in self.inventory if item["available"]),
                "derived": sum(1 for item in self.inventory if item["kind"] == "D"),
                "raw": sum(1 for item in self.inventory if item["kind"] in ("A", "C")),
                "total": len(self.inventory),
                "unavailable": sum(1 for item in self.inventory if not item["available"]),
            },
            "manifestId": self.state.manifest_id,
            "schema": "FSTL-phase2-dashboard-v1",
            "synchronizationConditions": synchronization_conditions,
            "synchronized": synchronized,
        }


@dataclass
class ConsoleState:
    status: str = "Synchronizing"
    session_id: int = 0
    baseline: int = 0
    delta_sequence: int = 0
    last_state_us: int | None = None
    last_state_utc: str | None = None
    stale_reason: str | None = None
    stale_detected_us: int | None = None
    stale_detected_utc: str | None = None
    records: dict[str, dict[str, Any]] = field(default_factory=dict)
    baseline_records: dict[str, dict[str, Any]] = field(default_factory=dict)
    record_instances: dict[str, dict[str, Any]] = field(default_factory=dict)
    baseline_record_instances: dict[str, dict[str, Any]] = field(default_factory=dict)
    transactions: dict[int, dict[str, Any]] = field(default_factory=dict)
    manifest_id: int = 0
    required_manifest_id: int = 0
    manifest_records: dict[str, dict[str, Any]] = field(default_factory=dict)
    injections: list[dict[str, Any]] = field(default_factory=list)
    manifest_transactions: dict[int, dict[str, Any]] = field(default_factory=dict)
    hello_sent: bool = False
    hello_nonce: int | None = None
    hello_t0_us: int | None = None
    welcomed: bool = False
    session_begun: bool = False
    ended: bool = False
    clock_samples: list[tuple[int, int]] = field(default_factory=list)
    smoothed_offset_us: int | None = None
    clock_filter_valid: bool = False
    clock_filter_error: str | None = None
    manifest_applied: bool = False
    keyframe_applied: bool = False
    reliable_dependency_pending: bool = False

    def invalidate_clock_filter(self, reason: str) -> None:
        self.clock_samples.clear()
        self.smoothed_offset_us = None
        self.clock_filter_valid = False
        self.clock_filter_error = reason

    def add_clock_sample(self, t0: int, t1: int, t2: int, t3: int) -> bool:
        limit = (1 << 64) - 1
        if any(value < 0 or value > limit for value in (t0, t1, t2, t3)):
            self.invalidate_clock_filter("clock-overflow")
            return False
        if t2 < t1 or t3 < t0:
            self.clock_filter_error = "invalid-clock-sample"
            return False
        rtt = (t3 - t0) - (t2 - t1)
        if rtt < 0:
            self.clock_filter_error = "invalid-clock-sample"
            return False
        offset = _trunc_signed((t1 - t0) + (t2 - t3), 2)
        self.clock_samples.append((rtt, offset))
        self.clock_samples = self.clock_samples[-8:]
        candidate = min(self.clock_samples, key=lambda sample: sample[0])[1]
        if self.smoothed_offset_us is None:
            self.smoothed_offset_us = candidate
        else:
            self.smoothed_offset_us += _trunc_signed(
                candidate - self.smoothed_offset_us, 8
            )
        self.clock_filter_valid = True
        self.clock_filter_error = None
        return True

    def _apply_records(self, records: list[dict[str, Any]], replace: bool) -> None:
        if replace:
            self.records = {}
            self.record_instances = {}
        for record in records:
            self.records[record["recordName"]] = record["fields"]
            self.record_instances[_record_identity(record)] = copy.deepcopy(record)

    def end_session(self) -> None:
        """Publish a terminal tombstone then release all retained heavy state."""
        # A received terminal record is an explicit clean disconnect.  ``Stale``
        # remains reserved for abrupt silence past the configured age limit.
        self.status = "Disconnected"
        self.baseline = self.delta_sequence = 0
        self.last_state_us = None
        self.last_state_utc = None
        self.stale_reason = None
        self.stale_detected_us = None
        self.stale_detected_utc = None
        self.records.clear()
        self.baseline_records.clear()
        self.record_instances.clear()
        self.baseline_record_instances.clear()
        self.transactions.clear()
        self.manifest_records.clear()
        self.manifest_transactions.clear()
        self.manifest_id = 0
        self.required_manifest_id = 0
        self.manifest_applied = False
        self.keyframe_applied = False
        self.reliable_dependency_pending = False
        self.session_begun = False
        self.welcomed = False
        self.session_id = 0
        self.ended = True
        self.invalidate_clock_filter("session-ended")

    def apply(self, message_type: int, fields: dict[str, Any], payload: bytes,
              header: dict[str, int], at_us: int, at_utc: str) -> tuple[bool, list[dict[str, int]], list[dict[str, int]]]:
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
            self.invalidate_clock_filter("session-changed")
            self.session_id, self.welcomed, self.ended = header["session_id"], True, False
            self.add_clock_sample(
                int(fields["client_send_t0_us"]),
                int(fields["producer_receive_t1_us"]),
                int(fields["producer_send_t2_us"]),
                at_us,
            )
            return True, [], [header]
        if message_type == 4:  # SESSION_BEGIN
            if not self.welcomed or self.session_begun or header["session_id"] != self.session_id:
                raise ValueError("SESSION_BEGIN outside negotiated session")
            self.session_begun = True
            self.required_manifest_id = fields["required_manifest_id"]
            self.manifest_applied = self.required_manifest_id == 0
            self.keyframe_applied = False
            return True, [], [header]
        if message_type == 13:  # SESSION_END
            if not self.session_begun or header["session_id"] != self.session_id:
                raise ValueError("SESSION_END outside active session")
            self.end_session()
            return True, [], [header]
        if not self.session_begun or header["session_id"] != self.session_id:
            raise ValueError("state before negotiated SESSION_BEGIN")
        if message_type == 5:
            manifest = fields["manifest_id"]
            count, index = fields["part_count"], fields["part_index"]
            if not 1 <= count <= 64 or index >= count or fields["transaction_size"] > MAX_SNAPSHOT_TRANSACTION_BYTES:
                raise ValueError("invalid manifest transaction bounds")
            candidate = self.manifest_transactions.get(manifest)
            if candidate is None:
                if len(self.manifest_transactions) >= MAX_SNAPSHOT_TRANSACTIONS:
                    raise ValueError("manifest transaction quota")
                candidate = {
                    "manifest_kind": fields["manifest_kind"],
                    "part_count": count,
                    "producer_sample_time_us": fields["producer_sample_time_us"],
                    "transaction_sha256": fields["transaction_sha256"],
                    "transaction_size": fields["transaction_size"],
                    "parts": {},
                    "headers": {},
                }
                self.manifest_transactions[manifest] = candidate
            for key in ("manifest_kind", "part_count", "producer_sample_time_us",
                        "transaction_sha256", "transaction_size"):
                if candidate[key] != fields[key]:
                    self.manifest_transactions.pop(manifest, None)
                    raise ValueError("inconsistent manifest transaction")
            records_bytes = payload[56:]
            existing = candidate["parts"].get(index)
            if existing is not None and existing["records_bytes"] != records_bytes:
                self.manifest_transactions.pop(manifest, None)
                raise ValueError("conflicting manifest part")
            candidate["parts"][index] = {"fields": fields, "records_bytes": records_bytes}
            candidate["headers"][index] = header
            if sum(len(part["records_bytes"]) for part in candidate["parts"].values()) > candidate["transaction_size"]:
                self.manifest_transactions.pop(manifest, None)
                raise ValueError("manifest transaction size")
            if len(candidate["parts"]) != count:
                return False, [header], []
            merged: list[dict[str, Any]] = []
            for part in range(count):
                if part not in candidate["parts"]:
                    return False, [header], []
                merged.extend(candidate["parts"][part]["fields"]["records"])
            concatenated = b"".join(candidate["parts"][part]["records_bytes"] for part in range(count))
            if (len(concatenated) != candidate["transaction_size"] or
                    hashlib.sha256(concatenated).hexdigest() != candidate["transaction_sha256"]):
                self.manifest_transactions.pop(manifest, None)
                raise ValueError("manifest transaction hash")
            if any(record["recordName"] not in ("CLASS_MANIFEST", "WEAPON_MANIFEST")
                   for record in merged):
                self.manifest_transactions.pop(manifest, None)
                raise ValueError("invalid manifest record")
            self.manifest_records = {_record_identity(record): copy.deepcopy(record) for record in merged}
            self.manifest_id = manifest
            self.manifest_applied = self.required_manifest_id in (0, manifest)
            ack_headers = [candidate["headers"][part] for part in range(count)]
            self.manifest_transactions.clear()
            return True, [header], ack_headers
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
            if (candidate["required_manifest_id"] != 0 and
                    candidate["required_manifest_id"] != self.manifest_id):
                self.transactions.pop(snapshot, None)
                raise ValueError("snapshot references unapplied manifest")
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
            self.baseline_record_instances = copy.deepcopy(self.record_instances)
            self.delta_sequence = 0
            self.last_state_us = at_us
            self.last_state_utc = at_utc
            self.stale_reason = None
            self.stale_detected_us = None
            self.stale_detected_utc = None
            self.status = "Live"
            self.keyframe_applied = True
            ack_headers = [candidate["headers"][part] for part in range(count)]
            self.transactions.clear()
            return True, [header], ack_headers
        if message_type == 7:
            if fields["baseline_snapshot_id"] != self.baseline or fields["delta_sequence"] <= self.delta_sequence:
                # Phase 0: a bad baseline makes a live replica stale.  It only
                # becomes Synchronizing after the reliable resync is accepted.
                self.status = "Stale"
                self.stale_reason = "protocol-resync"
                self.stale_detected_us = None
                self.stale_detected_utc = None
                return False, [], []
            replica = copy.deepcopy(self.baseline_records)
            replica_instances = copy.deepcopy(self.baseline_record_instances)
            for record in fields["records"]:
                replica[record["recordName"]] = record["fields"]
                replica_instances[_record_identity(record)] = copy.deepcopy(record)
            self.records = replica
            self.record_instances = replica_instances
            self.delta_sequence = fields["delta_sequence"]
            self.last_state_us = at_us
            self.last_state_utc = at_utc
            self.stale_reason = None
            self.stale_detected_us = None
            self.stale_detected_utc = None
            self.status = "Live"
            return True, [], []
        return False, [], []

    def stale_if_needed(self, at_us: int, at_utc: str, stale_us: int) -> None:
        if self.status == "Live" and self.last_state_us is not None and at_us - self.last_state_us > stale_us:
            self.status = "Stale"
            self.stale_reason = "silence"
            self.stale_detected_us = at_us
            self.stale_detected_utc = at_utc

    def transcript(self, at_us: int, at_utc: str, observation_clock: str) -> str:
        session = self.records.get("SESSION_STATE", {})
        mission = self.records.get("MISSION_STATE", {})
        player_id = session.get("observed_player_entity_id", "none")
        flight = self.records.get("FLIGHT_STATE", {})
        sample = int(flight.get("producer_sample_time_us", session.get("producer_sample_time_us", "0")))
        age = max(0, at_us - sample) if sample else 0
        last_state_age = max(0, at_us - self.last_state_us) if self.last_state_us is not None else None
        stale_duration = (max(0, at_us - self.stale_detected_us)
                          if self.status == "Stale" and self.stale_reason == "silence" and self.stale_detected_us is not None
                          else None)
        result = {
            "age_us": age, "baseline": self.baseline, "delta_sequence": self.delta_sequence,
            "last_live_age_us": last_state_age,
            "last_live_observed_monotonic_us": str(self.last_state_us) if self.last_state_us is not None else None,
            "last_live_observed_utc": self.last_state_utc,
            "mission_generation": mission.get("mission_generation", 0),
            "observation_clock": observation_clock,
            "observed_at_monotonic_us": str(at_us),
            "observed_at_utc": at_utc,
            "player": player_id, "pose": {"orientation": flight.get("orientation_local_to_world", []),
            "position": flight.get("position_world", [])}, "session": str(self.session_id),
            "status": self.status, "time_us": str(sample),
            "stale_detected_monotonic_us": str(self.stale_detected_us) if self.stale_detected_us is not None else None,
            "stale_detected_utc": self.stale_detected_utc,
            "stale_duration_us": stale_duration,
            "stale_reason": self.stale_reason if self.status == "Stale" else None,
            "angular_velocity": flight.get("rotational_velocity_local", []),
            "velocity": flight.get("velocity_world", []),
            "dashboard": DashboardProjection(
                self,
                at_us,
                self.smoothed_offset_us,
                self.clock_filter_valid,
            ).build(),
            "injections": copy.deepcopy(self.injections),
        }
        return json.dumps(result, sort_keys=True, separators=(",", ":"))


class ConsoleClient:
    def __init__(self, sender: socket.socket | None, stale_us: int,
                 drop_once_delta: bool = False) -> None:
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
        self.drop_once_delta = drop_once_delta

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
        self.state.reliable_dependency_pending = True
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
                self.state.reliable_dependency_pending = False
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
            self.state.reliable_dependency_pending = False
            if self.state.status == "Stale":
                self.state.status = "Synchronizing"
            return True
        return False

    def receive(self, datagram: bytes, at_us: int, at_utc: str) -> bool:
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
        if header["message_type"] == 7 and self.drop_once_delta:
            self.drop_once_delta = False
            self.state.injections.append({
                "action": "drop-once",
                "message": "delta",
                "message_id": header["message_id"],
            })
            # A replaceable delta receives no ACK and does not trigger resync.
            # The next cumulative delta or keyframe is sufficient to converge.
            return True
        changed, validated_headers, applied_headers = self.state.apply(
            header["message_type"], decoded["fields"], payload, header, at_us, at_utc)
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
            self.state.reliable_dependency_pending = False
        if header["message_type"] == 7 and not changed:
            self._resync(at_us)
        return changed


def replay(paths: list[Path], stale_us: int, drop_once_delta: bool = False) -> int:
    client = ConsoleClient(None, stale_us, drop_once_delta)
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
        observed_at_us, observed_at_utc = replay_observation(tick)
        changed = client.receive(packet, observed_at_us, observed_at_utc)
        # A rejected delta deliberately does not publish data, but its required
        # resync transition is observable proof-client state and belongs in the
        # deterministic transcript.
        if changed or (header["message_type"] == 7 and client.state.status in ("Stale", "Synchronizing")):
            print(client.state.transcript(observed_at_us, observed_at_utc, "replay-simulated"))
    if not client.terminal_published:
        observed_at_us = (client.state.last_state_us or 0) + stale_us + 1
        observed_at_us, observed_at_utc = replay_observation(observed_at_us)
        client.state.stale_if_needed(observed_at_us, observed_at_utc, stale_us)
        print(client.state.transcript(observed_at_us, observed_at_utc, "replay-simulated"))
    return 0


def live(host: str, port: int, seconds: float, stale_us: int,
         drop_once_delta: bool = False) -> int:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.1)
        sock.connect((host, port))
        client = ConsoleClient(sock, stale_us, drop_once_delta)
        client.begin()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            client.poll_reliable(now_us())
            try:
                data = sock.recv(MAX_DATAGRAM)
                received_at_us, received_at_utc = local_observation()
                if client.receive(data, received_at_us, received_at_utc):
                    observed_at_us, observed_at_utc = local_observation()
                    print(client.state.transcript(observed_at_us, observed_at_utc, "local"))
            except socket.timeout:
                pass
            observed_at_us, observed_at_utc = local_observation()
            client.state.stale_if_needed(observed_at_us, observed_at_utc, stale_us)
        if not client.terminal_published:
            observed_at_us, observed_at_utc = local_observation()
            print(client.state.transcript(observed_at_us, observed_at_utc, "local"))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="independent FSTL 1.1 observation console")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=42042)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--stale-ms", type=int, default=3000)
    parser.add_argument("--drop-once", choices=("delta",),
                        help="drop exactly one decoded replaceable message and record the injection")
    parser.add_argument("--replay", type=Path, nargs="+", help="raw datagrams for deterministic transcript")
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535 or args.seconds <= 0 or args.stale_ms < 1:
        parser.error("invalid port, duration or stale timeout")
    try:
        drop_once_delta = args.drop_once == "delta"
        return (replay(args.replay, args.stale_ms * 1000, drop_once_delta)
                if args.replay
                else live(args.host, args.port, args.seconds,
                          args.stale_ms * 1000, drop_once_delta))
    except (OSError, ValueError, reference.DecodeFailure) as exc:
        print(f"console error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

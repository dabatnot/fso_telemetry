#!/usr/bin/env python3
"""Shared independent FSTL 1.1 observation client core.

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
RECORD_FLAG_DELETE = 0x02


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
    first_seen_us: int = 0


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
    for name in ("manifest_generation", "class_id", "weapon_class_id",
                 "entity_id", "contact_entity_id", "subsystem_id", "event_id"):
        if name in fields:
            parts.append(f"{name}={fields[name]}")
    return "/".join(parts)


def _legacy_record_view(
    instances: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Build the historical one-record-per-name view deterministically.

    ``record_instances`` is authoritative.  The name-only mapping remains for
    existing Phase 1/2 consumers that address singleton records directly.
    """
    records: dict[str, dict[str, Any]] = {}
    for identity in sorted(instances):
        record = instances[identity]
        records[record["recordName"]] = record["fields"]
    return records


def _apply_state_mutations(
    instances: dict[str, dict[str, Any]],
    mutations: list[dict[str, Any]],
    *,
    allow_delete: bool,
) -> None:
    """Apply complete FSTL state atoms to an identity-keyed replica."""
    for record in mutations:
        identity = _record_identity(record)
        flags = int(record.get("recordFlags", 0))
        if flags & RECORD_FLAG_DELETE:
            if not allow_delete:
                raise ValueError("DELETE is not valid in a full snapshot")
            instances.pop(identity, None)
            continue
        instances[identity] = copy.deepcopy(record)


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
        "max(dynamic_max_hull-hull_strength,0)": (
            "p2.dashboard.hull-missing-hits.v1",
            ["wire:DAMAGE_STATE.hull_strength", "wire:DAMAGE_STATE.dynamic_max_hull"]),
        "clamp(1-hull_strength/dynamic_max_hull,0,1)": (
            "p2.dashboard.hull-damage-ratio.v1",
            ["wire:DAMAGE_STATE.hull_strength", "wire:DAMAGE_STATE.dynamic_max_hull"]),
        "hull_strength-guardian_threshold": (
            "p2.dashboard.guardian-margin.v1",
            ["wire:DAMAGE_STATE.hull_strength", "wire:DAMAGE_STATE.guardian_threshold"]),
        "sum(segment_current_hits)": (
            "p2.dashboard.shield-current-total.v1", ["wire:SHIELD_STATE.segment_current_hits"]),
        "sum(segment_max_hits)": (
            "p2.dashboard.shield-max-total.v1", ["wire:SHIELD_STATE.segment_max_hits"]),
        "clamp(sum(segment_current_hits)/sum(segment_max_hits),0,1)": (
            "p2.dashboard.shield-ratio.v1",
            ["wire:SHIELD_STATE.has_shields", "wire:SHIELD_STATE.segment_current_hits",
             "wire:SHIELD_STATE.segment_max_hits"]),
        "[clamp(current[i]/maximum[i],0,1) for each shield segment]": (
            "p2.dashboard.shield-segment-ratios.v1",
            ["wire:SHIELD_STATE.has_shields", "wire:SHIELD_STATE.segment_current_hits",
             "wire:SHIELD_STATE.segment_max_hits"]),
        "argmin(shield_segment_ratios)": (
            "p2.dashboard.shield-weakest-segment.v1",
            ["derived:p2.dashboard.shield-segment-ratios.v1"]),
        "min(shield_segment_ratios)": (
            "p2.dashboard.shield-weakest-ratio.v1",
            ["derived:p2.dashboard.shield-segment-ratios.v1"]),
        "sum(max(segment_max_hits[i]-segment_current_hits[i],0))": (
            "p2.dashboard.shield-deficit.v1",
            ["wire:SHIELD_STATE.has_shields", "wire:SHIELD_STATE.segment_current_hits",
             "wire:SHIELD_STATE.segment_max_hits"]),
        "shield_deficit/regeneration_per_s at current rate": (
            "p2.dashboard.shield-recharge-eta.v1",
            ["derived:p2.dashboard.shield-deficit.v1",
             "wire:SHIELD_STATE.regeneration_per_s"]),
        "clamp(current_hits/max_hits,0,1)": (
            "p2.dashboard.subsystem-integrity-ratio.v1",
            ["wire:SUBSYSTEM_STATE.current_hits", "wire:SUBSYSTEM_STATE.max_hits"]),
        "max_hits>0 && current_hits<=0": (
            "p2.dashboard.subsystem-destroyed.v1",
            ["wire:SUBSYSTEM_STATE.current_hits", "wire:SUBSYSTEM_STATE.max_hits"]),
        "max(max_hits-current_hits,0)": (
            "p2.dashboard.subsystem-missing-hits.v1",
            ["wire:SUBSYSTEM_STATE.current_hits", "wire:SUBSYSTEM_STATE.max_hits"]),
        "unavailable: Phase 2 wire lacks exact ets_properties applicability": (
            "p2.dashboard.ets-share-unavailable.v1",
            ["wire:ENERGY_STATE.ets_mode", "wire:ENERGY_STATE.ets_shields_index",
             "wire:ENERGY_STATE.ets_weapons_index", "wire:ENERGY_STATE.ets_engines_index"]),
        "clamp(weapon_energy_current/weapon_energy_max,0,1)": (
            "p2.dashboard.weapon-energy-ratio.v1",
            ["wire:ENERGY_STATE.weapon_energy_current", "wire:ENERGY_STATE.weapon_energy_max"]),
        "clamp(fuel_current/fuel_max,0,1)": (
            "p2.dashboard.fuel-ratio.v1",
            ["wire:PROPULSION_STATE.fuel_current",
             "wire:PROPULSION_STATE.fuel_max"]),
        "clamp(aggregate_engine_current_hits/aggregate_engine_max_hits,0,1)": (
            "p2.dashboard.engine-integrity-ratio.v1",
            ["wire:ENERGY_STATE.aggregate_engine_current_hits",
             "wire:ENERGY_STATE.aggregate_engine_max_hits"]),
        "fuel_current/consumption_per_s": (
            "p2.dashboard.afterburner-autonomy.v1",
            ["wire:PROPULSION_STATE.fuel_current",
             "wire:PROPULSION_STATE.consumption_per_s"]),
        "(fuel_max-fuel_current)/recovery_per_s while inactive": (
            "p2.dashboard.afterburner-recharge.v1",
            ["wire:PROPULSION_STATE.propulsion_flags",
             "wire:PROPULSION_STATE.fuel_current", "wire:PROPULSION_STATE.fuel_max",
             "wire:PROPULSION_STATE.recovery_per_s"]),
        "max(fuel_current-minimum_to_engage,0)": (
            "p2.dashboard.afterburner-usable-fuel.v1",
            ["wire:PROPULSION_STATE.fuel_current",
             "wire:PROPULSION_STATE.minimum_to_engage",
             "wire:CLASS_MANIFEST.afterburner.minimum_start_fuel"]),
        "closed afterburner readiness mapping": (
            "p2.dashboard.afterburner-readiness.v1",
            ["wire:PROPULSION_STATE.propulsion_flags",
             "wire:PROPULSION_STATE.fuel_current",
             "wire:PROPULSION_STATE.minimum_to_engage",
             "wire:PROPULSION_STATE.cooldown_remaining_us",
             "wire:PROPULSION_STATE.recovery_per_s",
             "wire:CLASS_MANIFEST.afterburner.minimum_start_fuel"]),
        "max(cooldown_remaining_us/1e6,max(minimum_to_engage-fuel_current,0)/recovery_per_s)": (
            "p2.dashboard.afterburner-ready-delay.v1",
            ["wire:PROPULSION_STATE.propulsion_flags",
             "wire:PROPULSION_STATE.fuel_current",
             "wire:PROPULSION_STATE.minimum_to_engage",
             "wire:PROPULSION_STATE.cooldown_remaining_us",
             "wire:PROPULSION_STATE.recovery_per_s",
             "wire:CLASS_MANIFEST.afterburner.minimum_start_fuel"]),
        "clamp(ammunition.current/ammunition.initial,0,1)": (
            "p2.dashboard.ammo-ratio.v1",
            ["wire:WEAPON_STATE.ammunition.current",
             "wire:WEAPON_STATE.ammunition.initial"]),
        "clamp(tertiary.ammunition_current/tertiary.ammunition_initial,0,1)": (
            "p2.dashboard.tertiary-ammo-ratio.v1",
            ["wire:WEAPON_STATE.tertiary.ammunition_current",
             "wire:WEAPON_STATE.tertiary.ammunition_initial"]),
        "clamp(countermeasure.current/countermeasure.maximum,0,1)": (
            "p2.dashboard.countermeasure-ratio.v1",
            ["wire:WEAPON_STATE.countermeasure.current",
             "wire:WEAPON_STATE.countermeasure.maximum"]),
        "cooldown_remaining_us/1e6": (
            "p2.dashboard.weapon-cooldown-seconds.v1",
            ["wire:WEAPON_STATE.cooldown_remaining_us"]),
        "rearm_remaining_us/1e6": (
            "p2.dashboard.weapon-rearm-seconds.v1",
            ["wire:WEAPON_STATE.ammunition.rearm_remaining_us"]),
        "closed weapon bank availability mapping": (
            "p2.dashboard.weapon-bank-state.v1",
            ["wire:WEAPON_STATE.weapon_flags",
             "wire:WEAPON_STATE.ammunition.current",
             "wire:WEAPON_STATE.cooldown_remaining_us"]),
        "closed tertiary bank availability mapping": (
            "p2.dashboard.tertiary-bank-state.v1",
            ["wire:WEAPON_STATE.tertiary.ammunition_current",
             "wire:WEAPON_STATE.tertiary.cooldown_remaining_us"]),
        "closed countermeasure availability mapping": (
            "p2.dashboard.countermeasure-state.v1",
            ["wire:WEAPON_STATE.countermeasure.flags",
             "wire:WEAPON_STATE.countermeasure.current",
             "wire:WEAPON_STATE.countermeasure.cooldown_remaining_us"]),
        "1e6/fire.wait_us": (
            "p2.dashboard.weapon-nominal-rate.v1",
            ["wire:WEAPON_MANIFEST.fire.wait_us"]),
        "clamp(elapsed_us/required_us,0,1)": (
            "p2.dashboard.cargo-progress-ratio.v1",
            ["wire:CARGO_SCAN_STATE.elapsed_us",
             "wire:CARGO_SCAN_STATE.required_us"]),
        "max(required_us-elapsed_us,0)": (
            "p2.dashboard.cargo-remaining-us.v1",
            ["wire:CARGO_SCAN_STATE.elapsed_us",
             "wire:CARGO_SCAN_STATE.required_us"]),
        "norm(support.position_world-subject.position_world)": (
            "p2.dashboard.support-distance.v1",
            ["wire:SUPPORT_STATE.support_entity_id",
             "wire:FLIGHT_STATE.position_world"]),
        "norm(support.velocity_world-subject.velocity_world)": (
            "p2.dashboard.support-relative-speed.v1",
            ["wire:SUPPORT_STATE.support_entity_id",
             "wire:FLIGHT_STATE.velocity_world"]),
        "-dot(relative_velocity,unit_separation)": (
            "p2.dashboard.support-closing-speed.v1",
            ["wire:SUPPORT_STATE.support_entity_id",
             "wire:FLIGHT_STATE.position_world",
             "wire:FLIGHT_STATE.velocity_world"]),
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
        "norm(track.position_world-owner.position_world)": (
            "p3.dashboard.track-distance.v1",
            ["wire:RADAR_CONTACTS.position_world", "wire:FLIGHT_STATE.position_world"]),
        "norm(track.velocity_world-owner.velocity_world)": (
            "p3.dashboard.track-relative-speed.v1",
            ["wire:RADAR_CONTACTS.velocity_world", "wire:FLIGHT_STATE.velocity_world"]),
        "-dot(track_relative_velocity,unit_track_separation)": (
            "p3.dashboard.track-closing-speed.v1",
            ["wire:RADAR_CONTACTS.position_world", "wire:RADAR_CONTACTS.velocity_world",
             "wire:FLIGHT_STATE.position_world", "wire:FLIGHT_STATE.velocity_world"]),
        "track.position_world-owner.position_world": (
            "p3.dashboard.track-relative-position.v1",
            ["wire:RADAR_CONTACTS.position_world", "wire:FLIGHT_STATE.position_world"]),
        "atan2(track_relative_position.y,track_relative_position.x)": (
            "p3.dashboard.track-bearing.v1",
            ["derived:p3.dashboard.track-relative-position.v1"]),
        "atan2(track_relative_position.z,hypot(x,y))": (
            "p3.dashboard.track-elevation.v1",
            ["derived:p3.dashboard.track-relative-position.v1"]),
        "distance/closing_speed when closing_speed>0": (
            "p3.dashboard.track-ttc.v1",
            ["derived:p3.dashboard.track-distance.v1",
             "derived:p3.dashboard.track-closing-speed.v1"]),
        "norm(navpoint.position_world-owner.position_world)": (
            "p3.dashboard.navpoint-distance.v1",
            ["wire:NAVIGATION_STATE.navpoints.position_world",
             "wire:FLIGHT_STATE.position_world"]),
        "distance/projected_velocity_toward_navpoint when positive": (
            "p3.dashboard.navpoint-eta.v1",
            ["derived:p3.dashboard.navpoint-distance.v1",
             "wire:FLIGHT_STATE.velocity_world"]),
        "clamp(sensor_current_hits/sensor_max_hits,0,1)": (
            "p3.dashboard.sensor-integrity-ratio.v1",
            ["wire:RADAR_STATE.sensor_current_hits", "wire:RADAR_STATE.sensor_max_hits"]),
        "conjugate(owner.orientation_local_to_world) * track_relative_position": (
            "p3.dashboard.track-relative-position-local.v1",
            ["wire:RADAR_CONTACTS.position_world",
             "wire:FLIGHT_STATE.position_world",
             "wire:FLIGHT_STATE.orientation_local_to_world"]),
        "atan2(track_relative_position_local.x,track_relative_position_local.z)": (
            "p3.dashboard.track-local-bearing.v1",
            ["derived:p3.dashboard.track-relative-position-local.v1"]),
        "atan2(track_relative_position_local.y,hypot(x,z))": (
            "p3.dashboard.track-local-elevation.v1",
            ["derived:p3.dashboard.track-relative-position-local.v1"]),
        "radar-v2-v4: normalize(local.x,local.y) * acos(local.z/projection_distance)/pi": (
            "p3.dashboard.track-scope-position.v2",
            ["wire:RADAR_CONTACTS.radar_local_position",
             "wire:RADAR_CONTACTS.radar_projection_distance"]),
        "radar-v1-compat: normalize(local.x,local.y) * acos(local.z/distance)/pi": (
            "p3.dashboard.track-scope-position.v1-compat",
            ["wire:RADAR_CONTACTS.position_world",
             "wire:FLIGHT_STATE.position_world",
             "wire:FLIGHT_STATE.orientation_local_to_world"]),
        "hypot(radar_local_position.x,radar_local_position.y)>=0.01": (
            "p3.dashboard.track-scope-direction-defined.v1",
            ["wire:RADAR_CONTACTS.radar_local_position"]),
        "track_distance<=selected_range": (
            "p3.dashboard.track-scope-in-range.v1",
            ["derived:p3.dashboard.track-distance.v1",
             "wire:RADAR_STATE.selected_range"]),
        "RADAR_CONTACTS v2-v4 publication is authoritative": (
            "p3.dashboard.track-scope-published.v2",
            ["wire:RADAR_CONTACTS"]),
        "RADAR_CONTACTS v4 radar visual is authoritative": (
            "p3.dashboard.track-radar-visual.v4",
            ["wire:RADAR_CONTACTS.radar_blip_color",
             "wire:RADAR_CONTACTS.radar_blip_type"]),
        "missing-authoritative-radar-visual": (
            "p3.dashboard.track-radar-visual.v4-missing",
            ["wire:RADAR_CONTACTS.radar_blip_color",
             "wire:RADAR_CONTACTS.radar_blip_type"]),
        "directional radar projection (already bounded to the scope disk)": (
            "p3.dashboard.track-scope-clamped-position.v1",
            ["derived:p3.dashboard.track-scope-position.v1"]),
        "exact_hud_distance if present else geometric_track_distance": (
            "p3.dashboard.target-distance.v1",
            ["wire:TARGET_STATE.exact_hud_distance",
             "derived:p3.dashboard.track-distance.v1"]),
        "target-state-v2 exact HUD display distance": (
            "p3.dashboard.target-distance.v2",
            ["wire:TARGET_STATE.exact_hud_distance"]),
        "legacy geometric_track_distance": (
            "p3.dashboard.target-distance.v1-compat",
            ["derived:p3.dashboard.track-distance.v1"]),
        "target-state-v2 exact HUD display speed": (
            "p3.dashboard.target-hud-speed.v2",
            ["wire:TARGET_STATE.exact_hud_speed"]),
        "missing-authoritative-target-hud-speed": (
            "p3.dashboard.target-hud-speed.v2-missing",
            ["wire:TARGET_STATE.exact_hud_speed"]),
        "TARGET_STATE v4 HUD target color is authoritative": (
            "p3.dashboard.target-hud-color.v4",
            ["wire:TARGET_STATE.hud_target_color"]),
        "missing-authoritative-target-hud-color": (
            "p3.dashboard.target-hud-color.v4-missing",
            ["wire:TARGET_STATE.hud_target_color"]),
        "TARGET_STATE v5 target subsystem HUD label is authoritative": (
            "p3.dashboard.target-subsystem-label.v5",
            ["wire:TARGET_STATE.hud_target_subsystem_label"]),
        "TARGET_STATE v5 lock subsystem HUD label is authoritative": (
            "p3.dashboard.lock-subsystem-label.v5",
            ["wire:TARGET_STATE.hud_lock_subsystem_label"]),
        "missing-authoritative-target-subsystem-label": (
            "p3.dashboard.target-subsystem-label.v5-missing",
            ["wire:TARGET_STATE.hud_target_subsystem_label"]),
        "missing-authoritative-lock-subsystem-label": (
            "p3.dashboard.lock-subsystem-label.v5-missing",
            ["wire:TARGET_STATE.hud_lock_subsystem_label"]),
        "clamp(1-time_to_lock_remaining_us/weapon.lock.time_us,0,1)": (
            "p3.dashboard.lock-progress.v1",
            ["wire:LOCK_STATE.locks.time_to_lock_remaining_us",
             "wire:WEAPON_STATE.current_secondary_bank_id",
             "wire:WEAPON_STATE.secondary_banks",
             "manifest:WEAPON_MANIFEST.lock.time_us"]),
        "missile.position_world-owner.position_world": (
            "p3.dashboard.missile-relative-position.v1",
            ["wire:THREAT_STATE.incoming_missiles.position_world",
             "wire:FLIGHT_STATE.position_world"]),
        "conjugate(owner.orientation_local_to_world) * missile_relative_position": (
            "p3.dashboard.missile-relative-position-local.v1",
            ["derived:p3.dashboard.missile-relative-position.v1",
             "wire:FLIGHT_STATE.orientation_local_to_world"]),
        "norm(missile.position_world-owner.position_world)": (
            "p3.dashboard.missile-distance.v1",
            ["wire:THREAT_STATE.incoming_missiles.position_world",
             "wire:FLIGHT_STATE.position_world"]),
        "norm(missile.velocity_world-owner.velocity_world)": (
            "p3.dashboard.missile-relative-speed.v1",
            ["wire:THREAT_STATE.incoming_missiles.velocity_world",
             "wire:FLIGHT_STATE.velocity_world"]),
        "-dot(missile_relative_velocity,unit_missile_separation)": (
            "p3.dashboard.missile-closing-speed.v1",
            ["wire:THREAT_STATE.incoming_missiles.position_world",
             "wire:THREAT_STATE.incoming_missiles.velocity_world",
             "wire:FLIGHT_STATE.position_world",
             "wire:FLIGHT_STATE.velocity_world"]),
        "missile_distance/missile_closing_speed when closing_speed>0": (
            "p3.dashboard.missile-ttc.v1",
            ["derived:p3.dashboard.missile-distance.v1",
             "derived:p3.dashboard.missile-closing-speed.v1"]),
        "primary_fire_threat_active ? 180ms : absent": (
            "p3.dashboard.hud-alert-primary-period.v1",
            ["wire:HUD_ALERT_STATE.primary_fire_threat_active"]),
        "missile_lock_state == ATTEMPT ? 180ms : ACQUIRED ? 90ms : absent": (
            "p3.dashboard.hud-alert-lock-period.v1",
            ["wire:HUD_ALERT_STATE.missile_lock_state"]),
        "HUD_ALERT_STATE warning group is authoritative": (
            "p3.dashboard.hud-alert-warning.v1",
            ["wire:HUD_ALERT_STATE.warning_kind",
             "wire:HUD_ALERT_STATE.warning_text",
             "wire:HUD_ALERT_STATE.warning_remaining_us",
             "wire:HUD_ALERT_STATE.warning_instance_id"]),
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

    def _record_envelopes(self, name: str) -> list[dict[str, Any]]:
        return [
            record for _, record in sorted(self.state.record_instances.items())
            if record["recordName"] == name
        ]

    def _per_entity(self, name: str) -> dict[str, dict[str, Any]]:
        return {str(record["entity_id"]): record for record in self._records(name)
                if "entity_id" in record}

    def _manifest_records(self, name: str) -> list[dict[str, Any]]:
        return [
            record["fields"] for _, record in sorted(self.state.manifest_records.items())
            if record["recordName"] == name
        ]

    def build(self) -> dict[str, Any]:
        for identity, record in sorted(self.state.record_instances.items()):
            self._raw_leaf(f"records.{identity}", record["fields"],
                           f"wire:{record['recordName']}:v{record['recordVersion']}")
        for identity, record in sorted(self.state.manifest_records.items()):
            self._raw_leaf(f"manifest.{identity}", record["fields"],
                           f"wire:{record['recordName']}:v{record['recordVersion']}")

        flights = self._per_entity("FLIGHT_STATE")
        estimated_producer_now = (
            self.at_us + self.smoothed_offset_us
            if self.offset_filter_valid and self.smoothed_offset_us is not None
            else None
        )
        if (estimated_producer_now is not None and
                not -(1 << 63) <= estimated_producer_now < (1 << 63)):
            estimated_producer_now = None
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

        for entity, radar in self._per_entity("RADAR_STATE").items():
            self._add_derived(
                f"entities.{entity}.sensor_integrity_ratio",
                "clamp(sensor_current_hits/sensor_max_hits,0,1)",
                _ratio(radar.get("sensor_current_hits"),
                       radar.get("sensor_max_hits")),
            )

        for entity, damage in self._per_entity("DAMAGE_STATE").items():
            current = _as_float(damage.get("hull_strength"))
            maximum = _as_float(damage.get("dynamic_max_hull"))
            ratio = _ratio(current, maximum)
            self._add_derived(
                f"entities.{entity}.hull_ratio",
                "clamp(hull_strength/dynamic_max_hull,0,1)",
                ratio,
            )
            valid_hull = current is not None and maximum is not None and maximum > 0.0
            self._add_derived(
                f"entities.{entity}.hull_missing_hits",
                "max(dynamic_max_hull-hull_strength,0)",
                {
                    "available": valid_hull,
                    "reason": None if valid_hull else "missing-or-nonpositive-denominator",
                    "value": max(float(maximum) - float(current), 0.0) if valid_hull else None,
                },
            )
            self._add_derived(
                f"entities.{entity}.hull_damage_ratio",
                "clamp(1-hull_strength/dynamic_max_hull,0,1)",
                {
                    "available": bool(ratio["available"]),
                    "reason": ratio["reason"],
                    "value": 1.0 - float(ratio["value"]) if ratio["available"] else None,
                },
            )
            guardian = _as_float(damage.get("guardian_threshold"))
            guardian_available = current is not None and guardian is not None
            self._add_derived(
                f"entities.{entity}.guardian_margin",
                "hull_strength-guardian_threshold",
                {
                    "available": guardian_available,
                    "reason": None if guardian_available else "guardian-absent",
                    "value": float(current) - float(guardian) if guardian_available else None,
                },
            )

        for entity, shield in self._per_entity("SHIELD_STATE").items():
            current = [_as_float(value) for value in shield.get("segment_current_hits", [])]
            maximum = [_as_float(value) for value in shield.get("segment_max_hits", [])]
            has_shields = bool(shield.get("has_shields"))
            valid = (
                has_shields
                and len(current) > 0
                and len(current) == len(maximum)
                and not any(value is None for value in (*current, *maximum))
                and all(float(value) > 0.0 for value in maximum if value is not None)
            )
            reason = None if valid else ("shields-absent" if not has_shields else "invalid-segments")
            current_total = sum(float(value) for value in current) if valid else None
            maximum_total = sum(float(value) for value in maximum) if valid else None
            self._add_derived(
                f"entities.{entity}.shield_current_total", "sum(segment_current_hits)",
                {"available": current_total is not None, "reason": reason,
                 "value": current_total},
            )
            self._add_derived(
                f"entities.{entity}.shield_max_total", "sum(segment_max_hits)",
                {"available": maximum_total is not None, "reason": reason,
                 "value": maximum_total},
            )
            ratio = (
                _ratio(current_total, maximum_total)
                if valid
                else {"available": False, "reason": reason, "value": None}
            )
            self._add_derived(
                f"entities.{entity}.shield_ratio",
                "clamp(sum(segment_current_hits)/sum(segment_max_hits),0,1)",
                ratio,
            )
            segment_ratios = (
                [min(1.0, max(0.0, float(now) / float(limit)))
                 for now, limit in zip(current, maximum)]
                if valid
                else None
            )
            self._add_derived(
                f"entities.{entity}.shield_segment_ratios",
                "[clamp(current[i]/maximum[i],0,1) for each shield segment]",
                {"available": valid, "reason": reason, "value": segment_ratios},
            )
            weakest_index = (
                min(range(len(segment_ratios)), key=segment_ratios.__getitem__)
                if segment_ratios
                else None
            )
            self._add_derived(
                f"entities.{entity}.shield_weakest_segment_index",
                "argmin(shield_segment_ratios)",
                {"available": weakest_index is not None, "reason": reason,
                 "value": weakest_index},
            )
            self._add_derived(
                f"entities.{entity}.shield_weakest_segment_ratio",
                "min(shield_segment_ratios)",
                {"available": weakest_index is not None, "reason": reason,
                 "value": segment_ratios[weakest_index] if weakest_index is not None else None},
            )
            deficit = (
                sum(max(float(limit) - float(now), 0.0)
                    for now, limit in zip(current, maximum))
                if valid
                else None
            )
            self._add_derived(
                f"entities.{entity}.shield_deficit",
                "sum(max(segment_max_hits[i]-segment_current_hits[i],0))",
                {"available": deficit is not None, "reason": reason, "value": deficit},
            )
            regeneration = _as_float(shield.get("regeneration_per_s"))
            if deficit is None:
                eta = {"available": False, "reason": reason, "value": None}
            elif deficit == 0.0:
                eta = {"available": True, "reason": None, "value": 0.0}
            elif regeneration is None or regeneration <= 0.0:
                eta = {
                    "available": False,
                    "reason": "shield-regeneration-unavailable",
                    "value": None,
                }
            else:
                eta = {"available": True, "reason": None, "value": deficit / regeneration}
            self._add_derived(
                f"entities.{entity}.shield_recharge_eta_s",
                "shield_deficit/regeneration_per_s at current rate",
                eta,
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
            valid_hits = current is not None and maximum is not None and maximum >= 0.0
            self._add_derived(
                f"{prefix}.missing_hits", "max(max_hits-current_hits,0)",
                {
                    "available": valid_hits,
                    "reason": None if valid_hits else "missing-hits",
                    "value": max(float(maximum) - float(current), 0.0) if valid_hits else None,
                },
            )

        identities = self._per_entity("SHIP_IDENTITY")
        classes = {
            str(record["class_id"]): record
            for record in self._manifest_records("CLASS_MANIFEST")
            if "class_id" in record
        }

        for entity, energy in self._per_entity("ENERGY_STATE").items():
            for group in ("shields", "weapons", "engines"):
                self._add_derived(
                    f"entities.{entity}.ets_share.{group}",
                    "unavailable: Phase 2 wire lacks exact ets_properties applicability",
                    {"available": False, "reason": "ets-applicability-not-on-wire", "value": None},
                )
            self._add_derived(
                f"entities.{entity}.weapon_energy_ratio",
                "clamp(weapon_energy_current/weapon_energy_max,0,1)",
                _ratio(energy.get("weapon_energy_current"), energy.get("weapon_energy_max")),
            )
            self._add_derived(
                f"entities.{entity}.engine_integrity_ratio",
                "clamp(aggregate_engine_current_hits/aggregate_engine_max_hits,0,1)",
                _ratio(
                    energy.get("aggregate_engine_current_hits"),
                    energy.get("aggregate_engine_max_hits"),
                ),
            )

        for entity, propulsion in self._per_entity("PROPULSION_STATE").items():
            identity = identities.get(entity, {})
            class_record = classes.get(str(identity.get("ship_class_id")), {})
            afterburner = class_record.get("afterburner", {})
            if not isinstance(afterburner, dict):
                afterburner = {}
            current = _as_float(propulsion.get("fuel_current"))
            maximum = _as_float(propulsion.get("fuel_max"))
            consumption = _as_float(
                propulsion.get("consumption_per_s", afterburner.get("burn_rate"))
            )
            recovery = _as_float(
                propulsion.get("recovery_per_s", afterburner.get("recover_rate"))
            )
            minimum = _as_float(
                propulsion.get("minimum_to_engage", afterburner.get("minimum_start_fuel"))
            )
            cooldown_us = _as_float(propulsion.get("cooldown_remaining_us", 0))
            try:
                flags = int(propulsion.get("propulsion_flags", 0))
            except (TypeError, ValueError):
                flags = -1

            self._add_derived(
                f"entities.{entity}.fuel_ratio",
                "clamp(fuel_current/fuel_max,0,1)",
                _ratio(current, maximum),
            )
            autonomy = (
                {"available": True, "reason": None, "value": current / consumption}
                if current is not None and consumption is not None and consumption > 0.0
                else {"available": False, "reason": "missing-or-nonpositive-consumption", "value": None}
            )
            self._add_derived(
                f"entities.{entity}.afterburner_autonomy_s",
                "fuel_current/consumption_per_s",
                autonomy,
            )
            active = flags >= 0 and bool(flags & 0x0004)
            recharge = (
                {"available": False, "reason": "afterburner-active", "value": None}
                if active
                else (
                    {
                        "available": True,
                        "reason": None,
                        "value": max(maximum - current, 0.0) / recovery,
                    }
                    if current is not None and maximum is not None
                    and recovery is not None and recovery > 0.0
                    else {
                        "available": False,
                        "reason": "missing-or-nonpositive-recovery",
                        "value": None,
                    }
                )
            )
            self._add_derived(
                f"entities.{entity}.afterburner_recharge_s",
                "(fuel_max-fuel_current)/recovery_per_s while inactive",
                recharge,
            )
            usable = (
                {"available": True, "reason": None, "value": max(current - minimum, 0.0)}
                if current is not None and minimum is not None
                else {"available": False, "reason": "missing-fuel-or-engagement-minimum", "value": None}
            )
            self._add_derived(
                f"entities.{entity}.afterburner_usable_fuel",
                "max(fuel_current-minimum_to_engage,0)",
                usable,
            )

            available = flags >= 0 and bool(flags & 0x0001)
            locked = flags >= 0 and bool(flags & 0x0002)
            ready_delay: dict[str, Any]
            readiness: dict[str, Any]
            if not available:
                ready_delay = {"available": False, "reason": "afterburner-unavailable", "value": None}
                readiness = {"available": False, "reason": "afterburner-unavailable", "value": None}
            elif active:
                ready_delay = {"available": True, "reason": None, "value": 0.0}
                readiness = {"available": True, "reason": None, "value": "ACTIF"}
            elif locked:
                ready_delay = {"available": False, "reason": "afterburner-locked", "value": None}
                readiness = {"available": True, "reason": None, "value": "VERROUILLÉ"}
            elif current is None or minimum is None or cooldown_us is None:
                ready_delay = {"available": False, "reason": "missing-readiness-input", "value": None}
                readiness = {"available": False, "reason": "missing-readiness-input", "value": None}
            elif current < minimum and (recovery is None or recovery <= 0.0):
                ready_delay = {
                    "available": False,
                    "reason": "below-minimum-without-recovery",
                    "value": None,
                }
                readiness = {
                    "available": False,
                    "reason": "below-minimum-without-recovery",
                    "value": None,
                }
            else:
                fuel_wait = (
                    max(minimum - current, 0.0) / recovery
                    if recovery is not None and recovery > 0.0
                    else 0.0
                )
                delay = max(max(cooldown_us, 0.0) / 1_000_000.0, fuel_wait)
                ready_delay = {"available": True, "reason": None, "value": delay}
                readiness = {
                    "available": True,
                    "reason": None,
                    "value": "PRÊT" if delay <= 0.0 else "ATTENTE",
                }
            self._add_derived(
                f"entities.{entity}.afterburner_ready_delay_s",
                "max(cooldown_remaining_us/1e6,max(minimum_to_engage-fuel_current,0)/recovery_per_s)",
                ready_delay,
            )
            self._add_derived(
                f"entities.{entity}.afterburner_readiness",
                "closed afterburner readiness mapping",
                readiness,
            )

        for weapon_class in self._manifest_records("WEAPON_MANIFEST"):
            weapon_class_id = weapon_class.get("weapon_class_id")
            fire = weapon_class.get("fire")
            wait_us = (
                _as_float(fire.get("wait_us"))
                if isinstance(fire, dict)
                else None
            )
            valid_rate = (
                weapon_class_id is not None
                and wait_us is not None
                and wait_us > 0.0
            )
            self._add_derived(
                f"weapon_classes.{weapon_class_id}.nominal_rate_hz",
                "1e6/fire.wait_us",
                {
                    "available": valid_rate,
                    "reason": None if valid_rate else "fire-group-absent-or-invalid",
                    "value": 1_000_000.0 / wait_us if valid_rate else None,
                },
            )

        for entity, weapon in self._per_entity("WEAPON_STATE").items():
            try:
                weapon_flags = int(weapon.get("weapon_flags", 0))
            except (TypeError, ValueError):
                weapon_flags = -1
            for family in ("primary_banks", "secondary_banks"):
                locked_mask = 0x0010 if family == "primary_banks" else 0x0020
                for index, bank in enumerate(weapon.get(family, [])):
                    prefix = f"entities.{entity}.{family}[{index}]"
                    ammunition = bank.get("ammunition")
                    ammunition = ammunition if isinstance(ammunition, dict) else None
                    ammo_ratio = (
                        _ratio(ammunition.get("current"), ammunition.get("initial"))
                        if ammunition is not None
                        else {
                            "available": False,
                            "reason": "bank-does-not-use-ammunition",
                            "value": None,
                        }
                    )
                    self._add_derived(
                        f"{prefix}.ammo_ratio",
                        "clamp(ammunition.current/ammunition.initial,0,1)",
                        ammo_ratio,
                    )
                    cooldown_us = _as_float(bank.get("cooldown_remaining_us"))
                    self._add_derived(
                        f"{prefix}.cooldown_s",
                        "cooldown_remaining_us/1e6",
                        {
                            "available": cooldown_us is not None,
                            "reason": None if cooldown_us is not None else "missing-cooldown",
                            "value": max(cooldown_us, 0.0) / 1_000_000.0
                            if cooldown_us is not None else None,
                        },
                    )
                    rearm_us = (
                        _as_float(ammunition.get("rearm_remaining_us"))
                        if ammunition is not None
                        else None
                    )
                    self._add_derived(
                        f"{prefix}.rearm_s",
                        "rearm_remaining_us/1e6",
                        {
                            "available": rearm_us is not None,
                            "reason": None if rearm_us is not None else "bank-not-rearming",
                            "value": max(rearm_us, 0.0) / 1_000_000.0
                            if rearm_us is not None else None,
                        },
                    )
                    current_ammo = (
                        _as_float(ammunition.get("current"))
                        if ammunition is not None
                        else None
                    )
                    state_available = weapon_flags >= 0 and cooldown_us is not None
                    if not state_available:
                        bank_state = {
                            "available": False,
                            "reason": "missing-bank-state-input",
                            "value": None,
                        }
                    elif weapon_flags & locked_mask:
                        bank_state = {"available": True, "reason": None, "value": "VERROUILLÃ‰E"}
                    elif current_ammo is not None and current_ammo <= 0.0:
                        bank_state = {"available": True, "reason": None, "value": "VIDE"}
                    elif cooldown_us > 0.0:
                        bank_state = {"available": True, "reason": None, "value": "RECHARGE"}
                    else:
                        bank_state = {"available": True, "reason": None, "value": "DISPONIBLE"}
                    self._add_derived(
                        f"{prefix}.state",
                        "closed weapon bank availability mapping",
                        bank_state,
                    )

            tertiary = weapon.get("tertiary")
            if isinstance(tertiary, dict):
                prefix = f"entities.{entity}.tertiary"
                self._add_derived(
                    f"{prefix}.ammo_ratio",
                    "clamp(tertiary.ammunition_current/tertiary.ammunition_initial,0,1)",
                    _ratio(
                        tertiary.get("ammunition_current"),
                        tertiary.get("ammunition_initial"),
                    ),
                )
                for source_name, derived_name, formula in (
                    ("cooldown_remaining_us", "cooldown_s", "cooldown_remaining_us/1e6"),
                    ("rearm_remaining_us", "rearm_s", "rearm_remaining_us/1e6"),
                ):
                    duration = _as_float(tertiary.get(source_name))
                    self._add_derived(
                        f"{prefix}.{derived_name}",
                        formula,
                        {
                            "available": duration is not None,
                            "reason": None if duration is not None else f"missing-{source_name}",
                            "value": max(duration, 0.0) / 1_000_000.0
                            if duration is not None else None,
                        },
                    )
                tertiary_current = _as_float(tertiary.get("ammunition_current"))
                tertiary_cooldown = _as_float(tertiary.get("cooldown_remaining_us"))
                if tertiary_current is None or tertiary_cooldown is None:
                    tertiary_state = {
                        "available": False,
                        "reason": "missing-tertiary-state-input",
                        "value": None,
                    }
                elif tertiary_current <= 0.0:
                    tertiary_state = {"available": True, "reason": None, "value": "VIDE"}
                elif tertiary_cooldown > 0.0:
                    tertiary_state = {"available": True, "reason": None, "value": "RECHARGE"}
                else:
                    tertiary_state = {
                        "available": True, "reason": None, "value": "DISPONIBLE"
                    }
                self._add_derived(
                    f"{prefix}.state",
                    "closed tertiary bank availability mapping",
                    tertiary_state,
                )

            countermeasure = weapon.get("countermeasure")
            if isinstance(countermeasure, dict):
                self._add_derived(
                    f"entities.{entity}.countermeasure.quantity_ratio",
                    "clamp(countermeasure.current/countermeasure.maximum,0,1)",
                    _ratio(countermeasure.get("current"), countermeasure.get("maximum")),
                )
                cooldown_us = _as_float(countermeasure.get("cooldown_remaining_us"))
                current = _as_float(countermeasure.get("current"))
                try:
                    flags = int(countermeasure.get("flags", 0))
                except (TypeError, ValueError):
                    flags = -1
                if flags < 0 or current is None or cooldown_us is None:
                    countermeasure_state = {
                        "available": False,
                        "reason": "missing-countermeasure-state-input",
                        "value": None,
                    }
                elif flags & 0x0002:
                    countermeasure_state = {
                        "available": True, "reason": None, "value": "VERROUILLÃ‰E"
                    }
                elif current <= 0.0:
                    countermeasure_state = {
                        "available": True, "reason": None, "value": "VIDE"
                    }
                elif cooldown_us > 0.0:
                    countermeasure_state = {
                        "available": True, "reason": None, "value": "RECHARGE"
                    }
                else:
                    countermeasure_state = {
                        "available": True, "reason": None, "value": "DISPONIBLE"
                    }
                self._add_derived(
                    f"entities.{entity}.countermeasure.state",
                    "closed countermeasure availability mapping",
                    countermeasure_state,
                )
                self._add_derived(
                    f"entities.{entity}.countermeasure.cooldown_s",
                    "cooldown_remaining_us/1e6",
                    {
                        "available": cooldown_us is not None,
                        "reason": None if cooldown_us is not None else "missing-cooldown",
                        "value": max(cooldown_us, 0.0) / 1_000_000.0
                        if cooldown_us is not None else None,
                    },
                )

        for entity, cargo in self._per_entity("CARGO_SCAN_STATE").items():
            elapsed = _as_float(cargo.get("elapsed_us"))
            required = _as_float(cargo.get("required_us"))
            valid_timing = (
                elapsed is not None and required is not None
                and elapsed >= 0.0 and required > 0.0
            )
            reason = None if valid_timing else "cargo-timing-absent-or-invalid"
            self._add_derived(
                f"entities.{entity}.cargo.progress_ratio",
                "clamp(elapsed_us/required_us,0,1)",
                {
                    "available": valid_timing,
                    "reason": reason,
                    "value": min(1.0, max(0.0, elapsed / required))
                    if valid_timing else None,
                },
            )
            self._add_derived(
                f"entities.{entity}.cargo.remaining_us",
                "max(required_us-elapsed_us,0)",
                {
                    "available": valid_timing,
                    "reason": reason,
                    "value": max(required - elapsed, 0.0)
                    if valid_timing else None,
                },
            )

        flights = self._per_entity("FLIGHT_STATE")

        def vector3(record: dict[str, Any] | None, field: str) -> list[float] | None:
            if record is None:
                return None
            values = [_as_float(value) for value in record.get(field, [])]
            if len(values) != 3 or any(value is None for value in values):
                return None
            return [float(value) for value in values if value is not None]

        for entity, support in self._per_entity("SUPPORT_STATE").items():
            support_entity = support.get("support_entity_id")
            subject_flight = flights.get(entity)
            support_flight = (
                flights.get(str(support_entity))
                if support_entity is not None else None
            )
            subject_position = vector3(subject_flight, "position_world")
            support_position = vector3(support_flight, "position_world")
            subject_velocity = vector3(subject_flight, "velocity_world")
            support_velocity = vector3(support_flight, "velocity_world")
            separation = (
                [remote - local for local, remote in zip(subject_position, support_position)]
                if subject_position is not None and support_position is not None
                else None
            )
            relative_velocity = (
                [remote - local for local, remote in zip(subject_velocity, support_velocity)]
                if subject_velocity is not None and support_velocity is not None
                else None
            )
            distance = (
                math.sqrt(sum(component * component for component in separation))
                if separation is not None else None
            )
            relative_speed = (
                math.sqrt(sum(component * component for component in relative_velocity))
                if relative_velocity is not None else None
            )
            same_entity = (
                support_entity is not None and str(support_entity) == str(entity)
            )
            closing_available = (
                distance is not None and relative_velocity is not None
                and (distance > 0.0 or same_entity)
            )
            closing_speed = None
            if closing_available:
                closing_speed = (
                    0.0 if distance == 0.0
                    else -sum(
                        velocity * axis / distance
                        for velocity, axis in zip(relative_velocity, separation)
                    )
                )
            missing_reason = (
                "support-entity-absent"
                if support_entity is None
                else "support-flight-state-absent-or-invalid"
            )
            self._add_derived(
                f"entities.{entity}.support.distance",
                "norm(support.position_world-subject.position_world)",
                {
                    "available": distance is not None,
                    "reason": None if distance is not None else missing_reason,
                    "value": distance,
                },
            )
            self._add_derived(
                f"entities.{entity}.support.relative_speed",
                "norm(support.velocity_world-subject.velocity_world)",
                {
                    "available": relative_speed is not None,
                    "reason": None if relative_speed is not None else missing_reason,
                    "value": relative_speed,
                },
            )
            self._add_derived(
                f"entities.{entity}.support.closing_speed",
                "-dot(relative_velocity,unit_separation)",
                {
                    "available": closing_available,
                    "reason": None if closing_available else (
                        "coincident-entities-without-direction"
                        if distance == 0.0 and not same_entity
                        else missing_reason
                    ),
                    "value": closing_speed,
                },
            )

        track_metrics: dict[tuple[str, str], dict[str, Any]] = {}
        radars = self._per_entity("RADAR_STATE")
        for contact_envelope in self._record_envelopes("RADAR_CONTACTS"):
            contact = contact_envelope["fields"]
            contact_record_version = int(contact_envelope["recordVersion"])
            owner = str(contact.get("entity_id", ""))
            contact_id = str(contact.get("contact_entity_id", ""))
            owner_flight = flights.get(owner)
            position = vector3(contact, "position_world")
            velocity = vector3(contact, "velocity_world")
            owner_position = vector3(owner_flight, "position_world")
            owner_velocity = vector3(owner_flight, "velocity_world")
            separation = ([remote - local for local, remote in zip(owner_position, position)]
                          if position is not None and owner_position is not None else None)
            relative_velocity = ([remote - local for local, remote in zip(owner_velocity, velocity)]
                                 if velocity is not None and owner_velocity is not None else None)
            distance = math.sqrt(sum(value * value for value in separation)) if separation is not None else None
            relative_speed = math.sqrt(sum(value * value for value in relative_velocity)) if relative_velocity is not None else None
            closing = (-sum(speed * axis / distance for speed, axis in zip(relative_velocity, separation))
                       if distance is not None and distance > 0.0 and relative_velocity is not None else None)
            prefix = f"entities.{owner}.tracks.{contact_id}"
            reason = "track-or-owner-flight-state-absent-or-invalid"
            relative_position_available = separation is not None
            bearing = (math.atan2(separation[1], separation[0])
                       if relative_position_available else None)
            horizontal_distance = (math.hypot(separation[0], separation[1])
                                   if relative_position_available else None)
            elevation = (math.atan2(separation[2], horizontal_distance)
                         if horizontal_distance is not None else None)
            local_separation = (
                _rotate_world_to_local(
                    owner_flight.get("orientation_local_to_world", [])
                    if owner_flight is not None else [],
                    separation,
                )
                if separation is not None else None
            )
            local_bearing = (
                math.atan2(local_separation[0], local_separation[2])
                if local_separation is not None else None
            )
            local_elevation = (
                math.atan2(
                    local_separation[1],
                    math.hypot(local_separation[0], local_separation[2]),
                )
                if local_separation is not None else None
            )
            selected_range = _as_float(radars.get(owner, {}).get("selected_range"))
            # Match the standard FSO radar: this is a directional projection,
            # not a top-down distance map. The nose is the centre; screen X is
            # local right/left and screen Y is local up/down. Radial distance is
            # the angle from the nose, normalized over the forward-to-rear
            # hemisphere. ``selected_range`` decides visibility, not placement.
            radar_local = vector3(contact, "radar_local_position")
            radar_distance = _as_float(
                contact.get("radar_projection_distance")
            )
            authoritative_radar_projection = (
                contact_record_version >= 2
                and radar_local is not None
                and radar_distance is not None
                and radar_distance >= 0.0
            )
            projection_local = (
                radar_local if authoritative_radar_projection
                else local_separation if contact_record_version == 1
                else None
            )
            projection_distance = (
                radar_distance if authoritative_radar_projection
                else distance if contact_record_version == 1
                else None
            )
            scope_position = None
            scope_direction_defined = None
            if (projection_local is not None and projection_distance is not None
                    and projection_distance > 0.0):
                transverse = math.hypot(
                    projection_local[0], projection_local[1]
                )
                forward_cosine = max(
                    -1.0,
                    min(1.0, projection_local[2] / projection_distance),
                )
                radial = (
                    0.0
                    if projection_distance < projection_local[2]
                    else math.acos(forward_cosine) / math.pi
                )
                scope_direction_defined = transverse >= 0.01
                scope_position = (
                    [
                        projection_local[0] * radial / transverse,
                        -projection_local[1] * radial / transverse,
                    ]
                    if scope_direction_defined else [0.0, 0.0]
                )
            scope_in_range = (
                True
                if contact_record_version >= 2
                else (
                    distance <= selected_range
                    if distance is not None and selected_range is not None
                    and selected_range > 0.0
                    else None
                )
            )
            scope_clamped = scope_position
            self._add_derived(f"{prefix}.relative_position",
                              "track.position_world-owner.position_world",
                              {"available": relative_position_available,
                               "reason": None if relative_position_available else reason,
                               "value": separation})
            self._add_derived(f"{prefix}.bearing_rad",
                              "atan2(track_relative_position.y,track_relative_position.x)",
                              {"available": bearing is not None,
                               "reason": None if bearing is not None else reason,
                               "value": bearing})
            self._add_derived(f"{prefix}.elevation_rad",
                              "atan2(track_relative_position.z,hypot(x,y))",
                              {"available": elevation is not None,
                               "reason": None if elevation is not None else reason,
                               "value": elevation})
            self._add_derived(
                f"{prefix}.relative_position_local",
                "conjugate(owner.orientation_local_to_world) * track_relative_position",
                {
                    "available": local_separation is not None,
                    "reason": None if local_separation is not None else reason,
                    "value": local_separation,
                },
            )
            self._add_derived(
                f"{prefix}.bearing_local_rad",
                "atan2(track_relative_position_local.x,track_relative_position_local.z)",
                {
                    "available": local_bearing is not None,
                    "reason": None if local_bearing is not None else reason,
                    "value": local_bearing,
                },
            )
            self._add_derived(
                f"{prefix}.elevation_local_rad",
                "atan2(track_relative_position_local.y,hypot(x,z))",
                {
                    "available": local_elevation is not None,
                    "reason": None if local_elevation is not None else reason,
                    "value": local_elevation,
                },
            )
            self._add_derived(
                f"{prefix}.scope_position",
                ("radar-v2-v4: normalize(local.x,local.y) * "
                 "acos(local.z/projection_distance)/pi"
                 if contact_record_version >= 2
                 else ("radar-v1-compat: normalize(local.x,local.y) * "
                       "acos(local.z/distance)/pi")),
                {
                    "available": scope_position is not None,
                    "reason": (
                        None if scope_position is not None
                        else "missing-authoritative-radar-projection"
                        if contact_record_version >= 2
                        else "missing-legacy-local-pose-or-nonzero-distance"
                    ),
                    "value": scope_position,
                },
            )
            self._add_derived(
                f"{prefix}.scope_direction_defined",
                "hypot(radar_local_position.x,radar_local_position.y)>=0.01",
                {
                    "available": scope_direction_defined is not None,
                    "reason": (
                        None if scope_direction_defined is not None
                        else "missing-scope-position"
                    ),
                    "value": scope_direction_defined,
                },
            )
            self._add_derived(
                f"{prefix}.scope_in_range",
                ("RADAR_CONTACTS v2-v4 publication is authoritative"
                 if contact_record_version >= 2
                 else "track_distance<=selected_range"),
                {
                    "available": scope_in_range is not None,
                    "reason": None if scope_in_range is not None else "missing-track-distance-or-positive-range",
                    "value": scope_in_range,
                },
            )
            self._add_derived(
                f"{prefix}.scope_clamped_position",
                "directional radar projection (already bounded to the scope disk)",
                {
                    "available": scope_clamped is not None,
                    "reason": None if scope_clamped is not None else "missing-scope-position",
                    "value": scope_clamped,
                },
            )
            radar_color = contact.get("radar_blip_color")
            radar_blip_type = contact.get("radar_blip_type")
            authoritative_visual = (
                contact_record_version >= 4
                and isinstance(radar_color, list)
                and len(radar_color) == 4
                and all(isinstance(component, int) and 0 <= component <= 255
                        for component in radar_color)
                and isinstance(radar_blip_type, int)
                and 0 <= radar_blip_type <= 5
            )
            self._add_derived(
                f"{prefix}.radar_visual",
                ("RADAR_CONTACTS v4 radar visual is authoritative"
                 if authoritative_visual else "missing-authoritative-radar-visual"),
                {
                    "available": authoritative_visual,
                    "reason": None if authoritative_visual else "missing-authoritative-color",
                    "value": ({"color": radar_color, "blip_type": radar_blip_type}
                              if authoritative_visual else None),
                },
            )
            self._add_derived(f"{prefix}.distance", "norm(track.position_world-owner.position_world)",
                              {"available": distance is not None, "reason": None if distance is not None else reason, "value": distance})
            self._add_derived(f"{prefix}.relative_speed", "norm(track.velocity_world-owner.velocity_world)",
                              {"available": relative_speed is not None, "reason": None if relative_speed is not None else reason, "value": relative_speed})
            self._add_derived(f"{prefix}.closing_speed", "-dot(track_relative_velocity,unit_track_separation)",
                              {"available": closing is not None, "reason": None if closing is not None else reason, "value": closing})
            ttc_s = distance / closing if closing is not None and closing > 0.0 else None
            self._add_derived(f"{prefix}.ttc_s", "distance/closing_speed when closing_speed>0",
                              {"available": ttc_s is not None,
                               "reason": None if ttc_s is not None else "track-not-approaching-or-invalid",
                               "value": ttc_s})
            sample_time = _as_float(contact.get("producer_sample_time_us"))
            track_age_us = (
                estimated_producer_now - sample_time
                if sample_time is not None and estimated_producer_now is not None
                and estimated_producer_now >= sample_time else None
            )
            self._add_derived(f"{prefix}.age_us",
                              "client_monotonic_time_us+smoothed_offset-producer_sample_time_us",
                              {"available": track_age_us is not None,
                               "reason": None if track_age_us is not None else "invalid-or-missing-clock-offset",
                               "value": track_age_us})
            track_metrics[(owner, contact_id)] = {
                "distance": distance,
                "closing_speed": closing,
                "relative_speed": relative_speed,
            }

        for target_envelope in self._record_envelopes("TARGET_STATE"):
            target = target_envelope["fields"]
            entity = str(target.get("entity_id", ""))
            target_id = str(target.get("current_target_entity_id", "0"))
            target_record_version = int(target_envelope["recordVersion"])
            exact_distance = _as_float(target.get("exact_hud_distance"))
            geometric_distance = track_metrics.get((entity, target_id), {}).get("distance")
            exact_speed = _as_float(target.get("exact_hud_speed"))
            effective_distance = (
                exact_distance if target_record_version >= 2
                else exact_distance if exact_distance is not None else geometric_distance
            )
            self._add_derived(
                f"entities.{entity}.target.distance",
                ("target-state-v2 exact HUD display distance"
                 if target_record_version >= 2 and exact_distance is not None
                 else "exact_hud_distance if present else geometric_track_distance"
                 if exact_distance is not None
                 else "legacy geometric_track_distance"),
                {
                    "available": effective_distance is not None,
                    "reason": None if effective_distance is not None else "no-target-distance-source",
                    "value": effective_distance,
                },
            )
            self._add_derived(
                f"entities.{entity}.target.hud_speed",
                ("target-state-v2 exact HUD display speed"
                 if target_record_version >= 2 and exact_speed is not None
                 else "missing-authoritative-target-hud-speed"),
                {
                    "available": target_record_version >= 2 and exact_speed is not None,
                    "reason": None if target_record_version >= 2 and exact_speed is not None
                    else "legacy-target-state-has-no-authoritative-hud-speed",
                    "value": exact_speed if target_record_version >= 2 else None,
                },
            )
            hud_target_color = target.get("hud_target_color")
            authoritative_hud_color = (
                target_record_version >= 4
                and isinstance(hud_target_color, list)
                and len(hud_target_color) == 4
                and all(isinstance(component, int) and 0 <= component <= 255
                        for component in hud_target_color)
            )
            self._add_derived(
                f"entities.{entity}.target.hud_color",
                ("TARGET_STATE v4 HUD target color is authoritative"
                 if authoritative_hud_color
                 else "missing-authoritative-target-hud-color"),
                {
                    "available": authoritative_hud_color,
                    "reason": None if authoritative_hud_color else "missing-authoritative-color",
                    "value": hud_target_color if authoritative_hud_color else None,
                },
            )
            for field, suffix, present_provenance, missing_provenance in (
                ("hud_target_subsystem_label", "target_subsystem_label",
                 "TARGET_STATE v5 target subsystem HUD label is authoritative",
                 "missing-authoritative-target-subsystem-label"),
                ("hud_lock_subsystem_label", "lock_subsystem_label",
                 "TARGET_STATE v5 lock subsystem HUD label is authoritative",
                 "missing-authoritative-lock-subsystem-label"),
            ):
                label = target.get(field)
                authoritative_label = (
                    target_record_version >= 5
                    and isinstance(label, str)
                    and bool(label.strip())
                )
                self._add_derived(
                    f"entities.{entity}.target.{suffix}",
                    present_provenance if authoritative_label else missing_provenance,
                    {
                        "available": authoritative_label,
                        "reason": None if authoritative_label
                        else "missing-authoritative-subsystem-label",
                        "value": label if authoritative_label else None,
                    },
                )

        weapon_manifests = {
            str(record.get("weapon_class_id")): record
            for record in self._manifest_records("WEAPON_MANIFEST")
            if record.get("weapon_class_id") is not None
        }
        weapons = self._per_entity("WEAPON_STATE")
        for entity, lock_state in self._per_entity("LOCK_STATE").items():
            weapon = weapons.get(entity)
            selected_bank = None
            if weapon is not None:
                selected_id = str(weapon.get("current_secondary_bank_id", "0"))
                matches = [
                    bank for bank in weapon.get("secondary_banks", [])
                    if str(bank.get("bank_id")) == selected_id
                ]
                if len(matches) == 1:
                    selected_bank = matches[0]
            manifest = (
                weapon_manifests.get(str(selected_bank.get("weapon_class_id")))
                if selected_bank is not None else None
            )
            lock_group = manifest.get("lock") if isinstance(manifest, dict) else None
            lock_duration_us = (
                _as_float(lock_group.get("time_us"))
                if isinstance(lock_group, dict) else None
            )
            for index, lock in enumerate(lock_state.get("locks", [])):
                remaining_us = _as_float(lock.get("time_to_lock_remaining_us"))
                locked = lock.get("locked") is True
                valid_duration = lock_duration_us is not None and lock_duration_us > 0.0
                progress = (
                    1.0 if locked and valid_duration else
                    min(1.0, max(0.0, 1.0 - remaining_us / lock_duration_us))
                    if remaining_us is not None and valid_duration else None
                )
                reason = (
                    None if progress is not None else
                    "lock-attempt-absent" if remaining_us is None and not locked else
                    "selected-secondary-lock-manifest-unavailable"
                )
                self._add_derived(
                    f"entities.{entity}.locks[{index}].progress",
                    "clamp(1-time_to_lock_remaining_us/weapon.lock.time_us,0,1)",
                    {
                        "available": progress is not None,
                        "reason": reason,
                        "value": progress,
                    },
                )

        for entity, alert in self._per_entity("HUD_ALERT_STATE").items():
            primary_period = 180 if alert.get("primary_fire_threat_active") is True else None
            lock_state = int(alert.get("missile_lock_state", 0))
            lock_period = 180 if lock_state == 1 else 90 if lock_state == 2 else None
            self._add_derived(
                f"entities.{entity}.hud_alert.primary_blink_period_ms",
                "primary_fire_threat_active ? 180ms : absent",
                {
                    "available": primary_period is not None,
                    "reason": None if primary_period is not None else "primary-fire-threat-inactive",
                    "value": primary_period,
                },
            )
            self._add_derived(
                f"entities.{entity}.hud_alert.lock_blink_period_ms",
                "missile_lock_state == ATTEMPT ? 180ms : ACQUIRED ? 90ms : absent",
                {
                    "available": lock_period is not None,
                    "reason": None if lock_period is not None else "missile-lock-threat-inactive",
                    "value": lock_period,
                },
            )
            warning = None
            if int(alert.get("presence", 0)) & 0x01:
                warning = {
                    "kind": alert.get("warning_kind"),
                    "text": alert.get("warning_text"),
                    "remaining_us": alert.get("warning_remaining_us"),
                    "instance_id": alert.get("warning_instance_id"),
                }
            self._add_derived(
                f"entities.{entity}.hud_alert.warning",
                "HUD_ALERT_STATE warning group is authoritative",
                {
                    "available": warning is not None,
                    "reason": None if warning is not None else "no-active-hud-warning",
                    "value": warning,
                },
            )

        for entity, threat in self._per_entity("THREAT_STATE").items():
            owner_flight = flights.get(entity)
            owner_position = vector3(owner_flight, "position_world")
            owner_velocity = vector3(owner_flight, "velocity_world")
            owner_orientation = (
                owner_flight.get("orientation_local_to_world", [])
                if owner_flight is not None else []
            )
            for missile in threat.get("incoming_missiles", []):
                missile_id = str(missile.get("entity_id", ""))
                position = vector3(missile, "position_world")
                velocity = vector3(missile, "velocity_world")
                separation = (
                    [remote - local for local, remote in zip(owner_position, position)]
                    if owner_position is not None and position is not None else None
                )
                relative_velocity = (
                    [remote - local for local, remote in zip(owner_velocity, velocity)]
                    if owner_velocity is not None and velocity is not None else None
                )
                local_separation = (
                    _rotate_world_to_local(owner_orientation, separation)
                    if separation is not None else None
                )
                distance = (
                    math.sqrt(sum(value * value for value in separation))
                    if separation is not None else None
                )
                relative_speed = (
                    math.sqrt(sum(value * value for value in relative_velocity))
                    if relative_velocity is not None else None
                )
                closing = (
                    -sum(speed * axis / distance for speed, axis in zip(relative_velocity, separation))
                    if distance is not None and distance > 0.0 and relative_velocity is not None
                    else None
                )
                ttc_s = distance / closing if closing is not None and closing > 0.0 else None
                prefix = f"entities.{entity}.missiles.{missile_id}"
                reason = "missile-or-owner-flight-state-absent-or-invalid"
                for path, formula, value in (
                    ("relative_position", "missile.position_world-owner.position_world", separation),
                    ("relative_position_local",
                     "conjugate(owner.orientation_local_to_world) * missile_relative_position",
                     local_separation),
                    ("distance", "norm(missile.position_world-owner.position_world)", distance),
                    ("relative_speed", "norm(missile.velocity_world-owner.velocity_world)",
                     relative_speed),
                    ("closing_speed",
                     "-dot(missile_relative_velocity,unit_missile_separation)", closing),
                ):
                    self._add_derived(
                        f"{prefix}.{path}",
                        formula,
                        {
                            "available": value is not None,
                            "reason": None if value is not None else reason,
                            "value": value,
                        },
                    )
                self._add_derived(
                    f"{prefix}.ttc_s",
                    "missile_distance/missile_closing_speed when closing_speed>0",
                    {
                        "available": ttc_s is not None,
                        "reason": None if ttc_s is not None else "missile-not-approaching-or-invalid",
                        "value": ttc_s,
                    },
                )

        for entity, navigation in self._per_entity("NAVIGATION_STATE").items():
            owner_flight = flights.get(entity)
            owner_position = vector3(owner_flight, "position_world")
            owner_velocity = vector3(owner_flight, "velocity_world")
            for navpoint in navigation.get("navpoints", []):
                navpoint_id = str(navpoint.get("navpoint_id", ""))
                navpoint_position = vector3(navpoint, "position_world")
                separation = (
                    [remote - local for local, remote in zip(owner_position, navpoint_position)]
                    if owner_position is not None and navpoint_position is not None else None
                )
                distance = (math.sqrt(sum(value * value for value in separation))
                            if separation is not None else None)
                closing_speed = (
                    sum(velocity * axis / distance for velocity, axis in zip(owner_velocity, separation))
                    if distance is not None and distance > 0.0 and owner_velocity is not None else None
                )
                eta_s = distance / closing_speed if closing_speed is not None and closing_speed > 0.0 else None
                prefix = f"entities.{entity}.navpoints.{navpoint_id}"
                reason = "navpoint-or-owner-flight-state-absent-or-invalid"
                self._add_derived(f"{prefix}.distance",
                                  "norm(navpoint.position_world-owner.position_world)",
                                  {"available": distance is not None,
                                   "reason": None if distance is not None else reason,
                                   "value": distance})
                self._add_derived(f"{prefix}.eta_s",
                                  "distance/projected_velocity_toward_navpoint when positive",
                                  {"available": eta_s is not None,
                                   "reason": None if eta_s is not None else "navpoint-not-approaching-or-invalid",
                                   "value": eta_s})

        sample_periods = {
            "FLIGHT_STATE": math.ceil(1_000_000 / self.flight_hz),
            "CONTROL_STATE": math.ceil(1_000_000 / self.flight_hz),
        }
        default_system_period = math.ceil(1_000_000 / self.systems_hz)
        for identity, record in sorted(self.state.record_instances.items()):
            sample = record["fields"].get("producer_sample_time_us")
            sample_value = int(sample) if sample is not None else None
            period = sample_periods.get(
                record["recordName"],
                self.mission_heartbeat_ms * 1000
                if record["recordName"] in ("SESSION_STATE", "MISSION_STATE")
                else default_system_period,
            )
            valid_age = (estimated_producer_now is not None and sample_value is not None
                         and estimated_producer_now >= sample_value)
            age = estimated_producer_now - sample_value if valid_age else None
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
    reliable_reassembly_timeout_us: int = 5_000_000

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
        candidate = (
            {}
            if replace
            else copy.deepcopy(self.record_instances)
        )
        _apply_state_mutations(
            candidate, records, allow_delete=not replace
        )
        self.record_instances = candidate
        self.records = _legacy_record_view(self.record_instances)

    def _apply_cumulative_delta_records(
        self, records: list[dict[str, Any]]
    ) -> None:
        """Rebuild one cumulative delta from the immutable ACKed baseline."""
        replica_instances = copy.deepcopy(self.baseline_record_instances)
        _apply_state_mutations(replica_instances, records, allow_delete=True)
        self.record_instances = replica_instances
        self.records = _legacy_record_view(replica_instances)

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
            self.reliable_reassembly_timeout_us = (
                int(fields["reliable_reassembly_timeout_ms"]) * 1000
            )
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
            self._apply_cumulative_delta_records(fields["records"])
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
        # WELCOME and SESSION_BEGIN are reliable lifecycle messages.  Keep only
        # their active-session identities so exact retransmissions can be
        # acknowledged idempotently after their state transition was applied.
        self.applied_control_messages: dict[
            int, tuple[int, int, int, int]
        ] = {}

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
        expired = [
            fragment_key
            for fragment_key, fragment_set in self.fragments.items()
            if at_us >= fragment_set.first_seen_us
            and at_us - fragment_set.first_seen_us
            >= self.state.reliable_reassembly_timeout_us
        ]
        for fragment_key in expired:
            self.fragments.pop(fragment_key, None)
        key = (header["session_id"], header["message_id"])
        group = self.fragments.setdefault(
            key, FragmentSet(header, first_seen_us=at_us)
        )
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
        control_identity = (
            header["session_id"],
            header["message_id"],
            header["message_crc32"],
            header["fragment_count"],
        )
        if (
            header["message_type"] in (3, 4)
            and self.applied_control_messages.get(header["message_type"])
            == control_identity
        ):
            if header["flags"] & ACK_REQUIRED:
                self._ack(header, ACK_APPLIED)
            return False
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
        if changed and header["message_type"] in (3, 4):
            if header["message_type"] == 3:
                self.applied_control_messages.clear()
            self.applied_control_messages[header["message_type"]] = control_identity
        for ack_header in validated_headers:
            if ack_header["flags"] & ACK_REQUIRED:
                self._ack(ack_header, ACK_VALIDATED)
        for ack_header in applied_headers:
            if ack_header["flags"] & ACK_REQUIRED:
                self._ack(ack_header, ACK_APPLIED)
        if header["message_type"] == 13 and changed:
            # No fragment/reassembly survives a validated terminal publication.
            self.fragments.clear()
            self.applied_control_messages.clear()
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

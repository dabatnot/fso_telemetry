#!/usr/bin/env python3
"""Generate deterministic FSTL 1.0 message and record golden vectors.

This tool is deliberately independent from the production C++ codecs.  It
uses only explicit little-endian standard-library packing.  The companion
``fstl_reference_decoder.py`` does not import this module.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from typing import Any


SCHEMA = "FSTL-1.0"
PRODUCER_ID = 0x0102030405060708
SESSION_ID = 0x1122334455667788
CLIENT_NONCE = 0x8877665544332211
SAMPLE_TIME_US = 1_000_000


def u8(value: int) -> bytes:
    return struct.pack("<B", value)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value)


def i64(value: int) -> bytes:
    return struct.pack("<q", value)


def f32(value: float) -> bytes:
    if not math.isfinite(value):
        raise ValueError("golden fixtures require finite floats")
    if value == 0.0:
        value = 0.0
    return struct.pack("<f", value)


def text(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return u16(len(encoded)) + encoded


def vec3(x: float = 0.0, y: float = 0.0, z: float = 0.0) -> bytes:
    return f32(x) + f32(y) + f32(z)


def decimal_u64(value: int) -> str:
    return str(value)


def record_envelope(record_type: int, payload: bytes, flags: int = 0) -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError("record payload exceeds the v1 envelope")
    return u16(record_type) + u8(1) + u8(flags) + u16(len(payload)) + payload


def canonical_record(
    record_type: int, name: str, flags: int, payload: bytes, fields: dict[str, Any]
) -> dict[str, Any]:
    return {
        "fields": fields,
        "kind": "record",
        "recordFlags": flags,
        "recordLength": len(payload),
        "recordName": name,
        "recordType": record_type,
        "recordVersion": 1,
        "schema": SCHEMA,
    }


def build_records() -> dict[int, tuple[str, bytes, dict[str, Any]]]:
    records: dict[int, tuple[str, bytes, dict[str, Any]]] = {}

    def add(
        record_type: int,
        name: str,
        payload: bytes,
        fields: dict[str, Any],
        flags: int = 0,
    ) -> None:
        records[record_type] = (
            name,
            record_envelope(record_type, payload, flags),
            canonical_record(record_type, name, flags, payload, fields),
        )

    payload = (
        u64(0)
        + u64(PRODUCER_ID)
        + u64(SAMPLE_TIME_US)
        + u8(0)
        + u8(0)
        + u8(2)
        + u8(0)
        + u32(1)
        + u64(0)
        + u64(1)
        + u64(0)
        + u64(0)
    )
    add(
        1,
        "SESSION_STATE",
        payload,
        {
            "authority_mode": 0,
            "event_coverage_exact": "0",
            "event_coverage_state_derived": "0",
            "negotiated_capabilities": "0",
            "negotiated_capability_generation": 1,
            "presence": "0",
            "producer_id": decimal_u64(PRODUCER_ID),
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
            "session_phase": 2,
            "state_domain_coverage": "1",
            "visibility_mode": 0,
        },
    )

    payload = u64(0) + u32(1) + u8(0) + u8(0) + u16(0) + f32(1.0) + u64(SAMPLE_TIME_US)
    add(
        2,
        "MISSION_STATE",
        payload,
        {
            "mission_generation": 1,
            "paused": 0,
            "phase": 0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
            "time_compression": 1.0,
        },
    )

    payload = (
        u32(1)
        + u32(1)
        + u64(0)
        + text("ship")
        + u32(0)
        + u32(0)
        + f32(1.0)
        + vec3()
    )
    add(
        3,
        "CLASS_MANIFEST",
        payload,
        {
            "center_of_mass": [0.0, 0.0, 0.0],
            "class_id": 1,
            "internal_name": "ship",
            "manifest_generation": 1,
            "mass": 1.0,
            "presence": "0",
            "ship_type_id": 0,
            "species_id": 0,
        },
    )

    payload = (
        u32(1)
        + u32(1)
        + u64(0)
        + text("weapon")
        + u8(0)
        + u64(0)
        + f32(0.0)
        + f32(0.0)
        + f32(0.0)
        + u64(0)
        + f32(0.0)
    )
    add(
        4,
        "WEAPON_MANIFEST",
        payload,
        {
            "gravity_multiplier": 0.0,
            "internal_name": "weapon",
            "lifetime_us": "0",
            "manifest_generation": 1,
            "mass": 0.0,
            "max_speed": 0.0,
            "presence": "0",
            "subtype": 0,
            "velocity_inheritance": 0.0,
            "weapon_class_id": 1,
            "weapon_flags": "0",
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(8) + u8(1) + u32(0)
    add(
        5,
        "ENTITY_LIFECYCLE",
        payload,
        {
            "entity_id": "1",
            "lifecycle_flags": 0,
            "lifecycle_phase": 1,
            "object_type": 8,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
        },
    )

    payload = (
        u64(1)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + u32(1)
        + text("ship")
        + u32(0)
        + u32(0)
        + u32(0)
        + u16(0)
        + f32(0.0)
    )
    add(
        6,
        "SHIP_IDENTITY",
        payload,
        {
            "entity_id": "1",
            "iff_id": 0,
            "internal_name": "ship",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "radius": 0.0,
            "role_flags": 0,
            "ship_class_id": 1,
            "species_id": 0,
            "team_id": 0,
        },
    )

    payload = (
        u64(1)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + vec3()
        + f32(1.0)
        + f32(0.0)
        + f32(0.0)
        + f32(0.0)
        + vec3()
        + vec3()
        + f32(0.0)
        + u32(0)
    )
    add(
        7,
        "FLIGHT_STATE",
        payload,
        {
            "entity_id": "1",
            "orientation_local_to_world": [1.0, 0.0, 0.0, 0.0],
            "physics_mode_flags": 0,
            "position_world": [0.0, 0.0, 0.0],
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "radius": 0.0,
            "rotational_velocity_local": [0.0, 0.0, 0.0],
            "velocity_world": [0.0, 0.0, 0.0],
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + f32(0.0) * 6 + u8(1) + u32(0)
    add(
        8,
        "CONTROL_STATE",
        payload,
        {
            "bank": 0.0,
            "control_flags": 0,
            "control_mode": 1,
            "entity_id": "1",
            "forward": 0.0,
            "heading": 0.0,
            "pitch": 0.0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "sideways": 0.0,
            "vertical": 0.0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + f32(50.0) + f32(100.0) + u16(0)
    add(
        9,
        "DAMAGE_STATE",
        payload,
        {
            "dynamic_max_hull": 100.0,
            "entity_id": "1",
            "hull_strength": 50.0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "protection_flags": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) + u16(0) + u16(0)
    add(
        10,
        "SHIELD_STATE",
        payload,
        {
            "entity_id": "1",
            "has_shields": 0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
            "segment_count": 0,
            "segment_current_hits": [],
            "segment_max_hits": [],
        },
    )

    payload = (
        u64(1)
        + u32(2)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + u16(0)
        + u8(1)
        + f32(0.0)
        + f32(0.0)
        + u32(0)
    )
    add(
        11,
        "SUBSYSTEM_STATE",
        payload,
        {
            "canonical_index": 0,
            "current_hits": 0.0,
            "entity_id": "1",
            "max_hits": 0.0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "subsystem_flags": 0,
            "subsystem_id": 2,
            "type": 1,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) * 5
    add(
        12,
        "ENERGY_STATE",
        payload,
        {
            "entity_id": "1",
            "ets_engines_index": 0,
            "ets_mode": 0,
            "ets_shields_index": 0,
            "ets_weapons_index": 0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u16(0) + u16(0)
    add(
        13,
        "PROPULSION_STATE",
        payload,
        {
            "entity_id": "1",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "propulsion_flags": 0,
            "reserved": 0,
        },
    )

    payload = (
        u64(1)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + u16(0) * 4
        + u32(0) * 4
        + u16(0)
        + u16(0)
    )
    add(
        14,
        "WEAPON_STATE",
        payload,
        {
            "current_primary_bank_id": 0,
            "current_secondary_bank_id": 0,
            "current_tertiary_bank_id": 0,
            "entity_id": "1",
            "presence": "0",
            "primary_bank_count": 0,
            "primary_banks": [],
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
            "secondary_bank_count": 0,
            "secondary_banks": [],
            "tertiary_bank_count": 0,
            "weapon_flags": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u16(0)
    add(
        15,
        "LOCK_STATE",
        payload,
        {
            "entity_id": "1",
            "locks": [],
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u64(0)
    add(
        16,
        "TARGET_STATE",
        payload,
        {
            "current_target_entity_id": "0",
            "entity_id": "1",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
        },
    )

    payload = (
        u64(1)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + u8(0)
        + f32(0.0)
        + u8(0)
        + f32(0.0)
        + f32(1.0)
    )
    add(
        17,
        "RADAR_STATE",
        payload,
        {
            "entity_id": "1",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "radar_mode": 0,
            "selected_range": 0.0,
            "sensor_current_hits": 0.0,
            "sensor_max_hits": 1.0,
            "sensor_state": 0,
        },
    )

    payload = (
        u64(1)
        + u64(2)
        + u64(0)
        + u64(SAMPLE_TIME_US)
        + u8(0)
        + u8(0)
        + u8(0)
        + vec3()
        + vec3()
        + f32(0.0)
        + u32(0)
    )
    add(
        18,
        "RADAR_CONTACTS",
        payload,
        {
            "category": 0,
            "contact_entity_id": "2",
            "contact_flags": 0,
            "entity_id": "1",
            "object_type": 0,
            "position_world": [0.0, 0.0, 0.0],
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "radius": 0.0,
            "velocity_world": [0.0, 0.0, 0.0],
            "visibility": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) + u16(0)
    add(
        19,
        "THREAT_STATE",
        payload,
        {
            "entity_id": "1",
            "incoming_missiles": [],
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "threat_level": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(1) + u8(0)
    add(
        20,
        "CARGO_SCAN_STATE",
        payload,
        {
            "disclosure": 0,
            "entity_id": "1",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "scan_phase": 1,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) + u64(0) + u16(0)
    add(
        21,
        "DOCKING_STATE",
        payload,
        {
            "entity_id": "1",
            "group_leader_entity_id": "0",
            "phase": 0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "relations": [],
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) + u8(0) + bytes(3)
    add(
        22,
        "SUPPORT_STATE",
        payload,
        {
            "entity_id": "1",
            "phase": 0,
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": "000000",
            "support_flags": 0,
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u8(0) + u16(0)
    add(
        23,
        "NAVIGATION_STATE",
        payload,
        {
            "autopilot_state": 0,
            "entity_id": "1",
            "navpoints": [],
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
        },
    )

    payload = u64(1) + u64(0) + u64(SAMPLE_TIME_US) + u32(0)
    add(
        24,
        "EFFECT_STATE",
        payload,
        {
            "effect_flags": 0,
            "entity_id": "1",
            "presence": "0",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
        },
    )

    content_hash = bytes(range(1, 33))
    asset_id = int.from_bytes(content_hash[:8], "big")
    bundle_hash = bytes(range(0x80, 0xA0))
    logical_name = "pilot"
    file_path = "heads/pilot.png"
    entry_length = 72 + len(logical_name) + len(file_path)
    entry = (
        u16(entry_length)
        + u64(asset_id)
        + content_hash
        + u8(4)
        + u8(6)
        + u8(1)
        + u8(0)
        + u16(0)
        + u16(320)
        + u16(200)
        + u32(1)
        + u64(100_000)
        + u16(len(logical_name))
        + u16(len(file_path))
        + u32(0)
        + logical_name.encode("ascii")
        + file_path.encode("ascii")
    )
    payload = (
        u16(1)
        + u16(0)
        + bundle_hash
        + u8(len("identity"))
        + u8(len("1.0"))
        + u64(0)
        + u64(0)
        + u32(1)
        + u32(0)
        + u16(1)
        + b"identity"
        + b"1.0"
        + entry
    )
    add(
        25,
        "COMM_ASSET_MANIFEST",
        payload,
        {
            "bundle_hash": bundle_hash.hex(),
            "bundle_version": 1,
            "converter_id": "identity",
            "converter_version": "1.0",
            "entries": [
                {
                    "alpha_mode": 1,
                    "asset_id": decimal_u64(asset_id),
                    "content_hash": content_hash.hex(),
                    "delivered_format": 6,
                    "duration_us": "100000",
                    "entry_length": entry_length,
                    "file_path": file_path,
                    "frame_count": 1,
                    "frame_duration_count": 0,
                    "frame_durations_us": [],
                    "height": 200,
                    "logical_name": logical_name,
                    "source_format": 4,
                    "timing_mode": 0,
                    "width": 320,
                }
            ],
            "entry_count": 1,
            "first_asset_index": 0,
            "frame_asset_id": "0",
            "manifest_flags": 0,
            "placeholder_asset_id": "0",
            "total_asset_count": 1,
        },
    )

    payload = bytes(60)
    add(
        26,
        "COMM_VIEW_STATE",
        payload,
        {
            "active": 0,
            "animation_time_us": "0",
            "color_mode": 0,
            "duration_us": "0",
            "engine_message_id": 0,
            "head_asset_id": "0",
            "playback_id": "0",
            "playback_mode": 0,
            "playback_rate": 0.0,
            "producer_sample_time_us": "0",
            "sender_entity_id": "0",
            "stop_reason": 0,
        },
    )

    payload = (
        u64(1)
        + u8(1)
        + u8(0)
        + u8(0)
        + u8(0)
        + u64(1)
        + u32(1)
        + u64(1)
        + u64(asset_id)
        + u64(SAMPLE_TIME_US)
        + u64(0)
        + u64(100_000)
        + f32(1.0)
    )
    add(
        27,
        "COMM_VIEW_EVENT",
        payload,
        {
            "animation_time_us": "0",
            "color_mode": 0,
            "duration_us": "100000",
            "engine_message_id": 1,
            "event_id": "1",
            "event_kind": 1,
            "head_asset_id": decimal_u64(asset_id),
            "playback_id": "1",
            "playback_mode": 0,
            "playback_rate": 1.0,
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "sender_entity_id": "1",
            "stop_reason": 0,
        },
        flags=1,
    )

    item = u32(0) + u64(1) + u64(SAMPLE_TIME_US) + u16(1) + u16(4) + u64(1)
    payload = u16(1) + u8(1) + u16(len(item)) + item
    add(
        28,
        "EVENTS",
        payload,
        {
            "events": [
                {
                    "event_flags": 4,
                    "event_id": "1",
                    "item_presence": 0,
                    "item_size": len(item),
                    "item_version": 1,
                    "kind": 1,
                    "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
                    "subject_entity_id": "1",
                }
            ]
        },
        flags=1,
    )

    if sorted(records) != list(range(1, 29)):
        raise RuntimeError("record fixture catalogue is incomplete")
    return records


def canonical_message(
    message_type: int, name: str, flags: int, fields: dict[str, Any]
) -> dict[str, Any]:
    return {
        "fields": fields,
        "kind": "message-payload",
        "messageFlags": flags,
        "messageName": name,
        "messageType": message_type,
        "schema": SCHEMA,
    }


def build_messages(
    records: dict[int, tuple[str, bytes, dict[str, Any]]]
) -> dict[int, tuple[str, int, bytes, dict[str, Any], dict[str, Any]]]:
    messages: dict[int, tuple[str, int, bytes, dict[str, Any], dict[str, Any]]] = {}

    def add(
        message_type: int,
        name: str,
        payload: bytes,
        fields: dict[str, Any],
        flags: int = 0,
        context: dict[str, Any] | None = None,
    ) -> None:
        messages[message_type] = (
            name,
            flags,
            payload,
            canonical_message(message_type, name, flags, fields),
            context or {},
        )

    payload = (
        u64(PRODUCER_ID)
        + u16(7808)
        + u8(1)
        + u8(1)
        + u8(0)
        + u8(0)
        + u64(0)
        + u32(1)
        + u16(4)
        + b"FSTL"
    )
    add(
        1,
        "DISCOVERY",
        payload,
        {
            "advert_sequence": 1,
            "listen_port": 7808,
            "max_major": 1,
            "max_minor": 0,
            "min_major": 1,
            "min_minor": 0,
            "producer_capabilities": "0",
            "producer_id": decimal_u64(PRODUCER_ID),
            "producer_name": "FSTL",
        },
    )

    payload = (
        u64(CLIENT_NONCE)
        + u64(100_000)
        + u8(1)
        + u8(1)
        + u8(0)
        + u8(0)
        + u8(0)
        + bytes(3)
        + u64(0)
        + u16(1000)
        + u16(0)
        + u16(0)
    )
    add(
        2,
        "HELLO",
        payload,
        {
            "advertised_capabilities": "0",
            "client_nonce": decimal_u64(CLIENT_NONCE),
            "client_send_t0_us": "100000",
            "extension_count": 0,
            "extensions": [],
            "extensions_length": 0,
            "max_major": 1,
            "max_minor": 0,
            "min_major": 1,
            "min_minor": 0,
            "requested_heartbeat_ms": 1000,
            "requested_visibility_mode": 0,
            "reserved": "000000",
        },
    )

    payload = (
        u64(CLIENT_NONCE)
        + u64(100_000)
        + u64(100_100)
        + u64(100_200)
        + u8(0)
        + u8(1)
        + u8(0)
        + u8(0)
        + u64(0)
        + u64(0)
        + u16(1000)
        + u16(2000)
        + u16(0)
        + u16(0)
        + u64(PRODUCER_ID)
    )
    add(
        3,
        "WELCOME",
        payload,
        {
            "active_capabilities": "0",
            "client_nonce": decimal_u64(CLIENT_NONCE),
            "client_send_t0_us": "100000",
            "extension_count": 0,
            "extensions": [],
            "extensions_length": 0,
            "heartbeat_interval_ms": 1000,
            "producer_capabilities": "0",
            "producer_id": decimal_u64(PRODUCER_ID),
            "producer_receive_t1_us": "100100",
            "producer_send_t2_us": "100200",
            "reliable_reassembly_timeout_ms": 2000,
            "selected_major": 1,
            "selected_minor": 0,
            "selected_visibility_mode": 0,
            "status": 0,
        },
    )

    payload = u32(7) + u64(100_000) + u64(1) + u32(1) + u32(1)
    add(
        4,
        "SESSION_BEGIN",
        payload,
        {
            "initial_snapshot_id": 1,
            "mission_instance_id": "1",
            "producer_session_start_us": "100000",
            "required_manifest_id": 1,
            "session_flags": 7,
        },
        flags=2,
    )

    class_record = records[3][1]
    digest = hashlib.sha256(class_record).digest()
    payload = (
        u32(1)
        + u16(0)
        + u16(1)
        + u32(len(class_record))
        + digest
        + u64(SAMPLE_TIME_US)
        + u16(1)
        + u16(1)
        + class_record
    )
    add(
        5,
        "MANIFEST",
        payload,
        {
            "manifest_id": 1,
            "manifest_kind": 1,
            "part_count": 1,
            "part_index": 0,
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "record_count": 1,
            "records": [records[3][2]],
            "transaction_sha256": digest.hex(),
            "transaction_size": len(class_record),
        },
        flags=2,
    )

    snapshot_records = records[1][1] + records[2][1] + records[5][1]
    digest = hashlib.sha256(snapshot_records).digest()
    payload = (
        u32(1)
        + u16(0)
        + u16(1)
        + u32(len(snapshot_records))
        + digest
        + u64(SAMPLE_TIME_US)
        + u32(1)
        + u16(1)
        + u16(3)
        + snapshot_records
    )
    add(
        6,
        "FULL_SNAPSHOT",
        payload,
        {
            "part_count": 1,
            "part_index": 0,
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "record_count": 3,
            "records": [records[1][2], records[2][2], records[5][2]],
            "required_manifest_id": 1,
            "snapshot_flags": 1,
            "snapshot_id": 1,
            "transaction_sha256": digest.hex(),
            "transaction_size": len(snapshot_records),
        },
        flags=6,
    )

    mission_record = records[2][1]
    payload = (
        u32(1)
        + u32(1)
        + u64(SAMPLE_TIME_US + 1)
        + u16(1)
        + u16(0)
        + mission_record
    )
    add(
        7,
        "DELTA",
        payload,
        {
            "baseline_snapshot_id": 1,
            "delta_sequence": 1,
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US + 1),
            "record_count": 1,
            "records": [records[2][2]],
            "reserved": 0,
        },
    )

    events_record = records[28][1]
    payload = (
        u32(1)
        + u64(1)
        + u64(SAMPLE_TIME_US)
        + u8(2)
        + u8(0)
        + u16(1)
        + events_record
    )
    add(
        8,
        "EVENT_BATCH",
        payload,
        {
            "batch_id": 1,
            "delivery_class": 2,
            "first_event_id": "1",
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "record_count": 1,
            "records": [records[28][2]],
            "reserved": 0,
        },
        flags=2,
    )

    payload = u32(1) + u8(1) + bytes(3) + u64(100_000) + u64(0) + u64(0)
    add(
        9,
        "HEARTBEAT",
        payload,
        {
            "kind": 1,
            "origin_t0_us": "100000",
            "probe_id": 1,
            "receive_t1_us": "0",
            "reserved": "000000",
            "transmit_t2_us": "0",
        },
    )

    payload = u32(100) + u8(6) + u8(3) + u16(1) + u32(0x12345678)
    add(
        10,
        "ACK",
        payload,
        {
            "ack_flags": 3,
            "target_fragment_count": 1,
            "target_message_crc32": "0x12345678",
            "target_message_id": 100,
            "target_message_type": 6,
        },
    )

    payload = (
        u32(100)
        + u8(6)
        + u8(1)
        + u16(9)
        + u32(0x12345678)
        + u64(0)
        + u16(2)
        + u16(0)
        + b"\x01\x01"
    )
    add(
        11,
        "NACK",
        payload,
        {
            "bitmap_bytes": 2,
            "missing_bitmap": "0101",
            "needed_before_producer_time_us": "0",
            "reason": 1,
            "reserved": 0,
            "target_fragment_count": 9,
            "target_message_crc32": "0x12345678",
            "target_message_id": 100,
            "target_message_type": 6,
        },
    )

    payload = u32(1) + u8(1) + u8(2) + u16(0) + u32(1) + u32(1) + u64(200_000)
    add(
        12,
        "RESYNC_REQUEST",
        payload,
        {
            "client_send_time_us": "200000",
            "last_applied_delta_sequence": 1,
            "last_applied_snapshot_id": 1,
            "reason": 1,
            "request_flags": 2,
            "request_id": 1,
            "reserved": 0,
        },
        flags=2,
    )

    payload = u8(1) + u8(1) + u16(0) + u32(1) + u64(SAMPLE_TIME_US)
    add(
        13,
        "SESSION_END",
        payload,
        {
            "end_flags": 1,
            "last_snapshot_id": 1,
            "producer_sample_time_us": decimal_u64(SAMPLE_TIME_US),
            "reason": 1,
            "reserved": 0,
        },
        flags=2,
    )

    payload = (
        u32(1)
        + u16(1024)
        + u16(1024)
        + u16(960)
        + u16(960)
        + u16(20)
        + u16(15)
        + u32(8000)
        + u32(4000)
        + u32(5)
        + u32(9)
        + u32(15)
        + u32(3)
        + u8(1)
        + bytes(3)
    )
    add(
        14,
        "TARGET_VIDEO_SUBSCRIBE",
        payload,
        {
            "acceptable_render_profiles": 3,
            "max_bitrate_kbps": 8000,
            "max_fps": 20,
            "max_height": 1024,
            "max_width": 1024,
            "overlay_capabilities": 15,
            "preferred_bitrate_kbps": 4000,
            "preferred_fps": 15,
            "preferred_height": 960,
            "preferred_render_profile": 1,
            "preferred_width": 960,
            "request_id": 1,
            "reserved": "000000",
            "supported_h264_levels": 9,
            "supported_h264_profiles": 5,
        },
        flags=2,
    )

    payload = (
        u32(1)
        + u8(0)
        + u8(1)
        + u8(100)
        + u8(41)
        + u32(1)
        + u32(1)
        + u8(1)
        + u8(1)
        + u8(1)
        + u8(1)
        + u16(960)
        + u16(960)
        + u16(15)
        + u16(1)
        + u32(4000)
        + u16(1000)
        + u16(500)
    )
    add(
        15,
        "TARGET_VIDEO_CONFIG",
        payload,
        {
            "bitrate_kbps": 4000,
            "codec": 1,
            "codec_level": 41,
            "codec_profile": 100,
            "config_generation": 1,
            "fps_den": 1,
            "fps_num": 15,
            "gop_duration_ms": 1000,
            "height": 960,
            "idr_recovery_window_ms": 500,
            "overlay_mode": 1,
            "pixel_format": 1,
            "recovery_mode": 1,
            "render_profile": 1,
            "request_id": 1,
            "result": 0,
            "stream_id": 1,
            "width": 960,
        },
        flags=2,
    )

    # High profile, level 4.1, progressive 4:2:0 8-bit 960x960 SPS with
    # limited-range BT.709 VUI, followed by one PPS and one IDR slice.
    annex_b = bytes(
        [
            0x00, 0x00, 0x00, 0x01, 0x67,
            0x64, 0x00, 0x29, 0xAC, 0xE8, 0x0F, 0x01, 0xE6, 0x9A, 0x80, 0x80, 0x80, 0x81,
            0x00, 0x00, 0x00, 0x01, 0x68, 0x22,
            0x00, 0x00, 0x00, 0x01, 0x65, 0x33,
        ]
    )
    payload = (
        u32(1)
        + u32(1)
        + u32(1)
        + u64(2)
        + u64(SAMPLE_TIME_US)
        + u32(len(annex_b))
        + u16(1)
        + u16(0)
        + annex_b
    )
    add(
        16,
        "TARGET_VIDEO_FRAME",
        payload,
        {
            "annex_b_access_unit": annex_b.hex(),
            "config_generation": 1,
            "encoded_frame_size": len(annex_b),
            "presentation_time_us": decimal_u64(SAMPLE_TIME_US),
            "reserved": 0,
            "stream_id": 1,
            "target_entity_id": "2",
            "video_flags": 1,
            "video_frame_id": 1,
        },
        flags=8,
    )

    payload = u32(1) + u32(1) + u32(1) + u32(0) + u64(0) + u8(3) + bytes(7)
    add(
        17,
        "TARGET_VIDEO_KEYFRAME_REQUEST",
        payload,
        {
            "config_generation": 1,
            "last_decodable_frame_id": 0,
            "needed_before_producer_time_us": "0",
            "reason": 3,
            "request_id": 1,
            "reserved": "00000000000000",
            "stream_id": 1,
        },
        flags=2,
        context={"clockFilterValid": False},
    )

    payload = u32(1) + u32(1) + u32(1) + u8(1) + u8(0) + u16(0) + u64(0)
    add(
        18,
        "TARGET_VIDEO_STOP",
        payload,
        {
            "config_generation": 1,
            "producer_time_us": "0",
            "reason": 1,
            "request_id": 1,
            "reserved": 0,
            "stop_flags": 0,
            "stream_id": 1,
        },
        flags=2,
        context={"senderRole": "client"},
    )

    counters = [1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0]
    payload = b"".join(u32(value) for value in counters) + u16(1000) + u16(0)
    add(
        19,
        "TARGET_VIDEO_STATS",
        payload,
        {
            "buffered_duration_us": 0,
            "config_generation": 1,
            "decode_latency_us": 0,
            "decoded_frames": 1,
            "decoder_errors": 0,
            "dropped_frames": 0,
            "estimated_loss_ppm": 0,
            "highest_decoded_frame_id": 1,
            "highest_presented_frame_id": 1,
            "highest_received_frame_id": 1,
            "jitter_us": 0,
            "missing_fragments": 0,
            "presentation_latency_us": 0,
            "presented_frames": 1,
            "received_frames": 1,
            "report_id": 1,
            "report_interval_ms": 1000,
            "stats_flags": 0,
            "stream_id": 1,
        },
    )

    payload = u32(2) + u64(0) + u64(0) + u64(SAMPLE_TIME_US) + u8(1) + bytes(3) + u16(0) + u16(0)
    add(
        20,
        "CAPABILITY_UPDATE",
        payload,
        {
            "active_capabilities": "0",
            "advertised_capabilities": "0",
            "capability_generation": 2,
            "effective_time_us": decimal_u64(SAMPLE_TIME_US),
            "extension_count": 0,
            "extensions": [],
            "extensions_length": 0,
            "reason": 1,
            "reserved": "000000",
        },
        flags=2,
    )

    if sorted(messages) != list(range(1, 21)):
        raise RuntimeError("message fixture catalogue is incomplete")
    return messages


VALIDATION_ERROR_NAMES = {
    11: "UNKNOWN_MESSAGE_TYPE",
    12: "WRONG_DIRECTION",
    14: "ENDPOINT_MISMATCH",
    24: "TRUNCATED_PAYLOAD",
    25: "TRAILING_BYTES",
    26: "UNKNOWN_REQUIRED_RECORD",
    27: "UNSUPPORTED_RECORD_VERSION",
    28: "BAD_RECORD_LENGTH",
    29: "DUPLICATE_RECORD",
    30: "DUPLICATE_ITEM_KEY",
    31: "INVALID_UTF8",
    32: "STRING_TOO_LONG",
    33: "NON_FINITE_FLOAT",
    34: "OUT_OF_RANGE",
    35: "UNKNOWN_ENUM",
    36: "RESERVED_FLAG",
    38: "UNKNOWN_ENTITY",
    39: "STALE_BASELINE",
    40: "STALE_GENERATION",
    41: "MISSING_MANIFEST",
    42: "CAPABILITY_NOT_NEGOTIATED",
    44: "INVALID_STATE_TRANSITION",
    46: "RESOURCE_LIMIT",
}


def replaced(encoded: bytes, offset: int, replacement: bytes) -> bytes:
    if offset < 0 or offset + len(replacement) > len(encoded):
        raise RuntimeError("fixture mutation escapes its source buffer")
    return encoded[:offset] + replacement + encoded[offset + len(replacement) :]


def with_record_payload(record_type: int, payload: bytes, flags: int = 0) -> bytes:
    return record_envelope(record_type, payload, flags)


def build_invalid_records(
    records: dict[int, tuple[str, bytes, dict[str, Any]]]
) -> list[dict[str, Any]]:
    fixtures: list[dict[str, Any]] = []

    def add(
        name: str,
        category: str,
        record_type: int,
        encoded: bytes,
        error: int,
        notes: str,
        *,
        context: dict[str, Any] | None = None,
    ) -> None:
        if error not in VALIDATION_ERROR_NAMES:
            raise RuntimeError(f"unregistered fixture ValidationError {error}")
        fixtures.append(
            {
                "category": category,
                "context": context or {},
                "encoded": encoded,
                "error": error,
                "kind": "record",
                "name": name,
                "notes": notes,
                "recordFlags": encoded[3] if len(encoded) >= 4 else 0,
                "recordType": record_type,
                "recordVersion": encoded[2] if len(encoded) >= 3 else 1,
            }
        )

    mission = records[2][1]
    class_manifest = records[3][1]
    flight = records[7][1]
    cargo = records[20][1]
    comm_manifest = records[25][1]
    comm_event = records[27][1]

    add(
        "record_truncated_header",
        "record_truncated",
        2,
        mission[:5],
        24,
        "A record envelope shorter than six octets is truncated.",
    )
    add(
        "record_declared_length_exceeds_input",
        "record_length_excessive",
        2,
        replaced(mission, 4, u16(int.from_bytes(mission[4:6], "little") + 1)),
        28,
        "record_length declares one byte more than the authoritative input.",
    )
    add(
        "invalid_record_type_zero",
        "record_invalid_type",
        0,
        replaced(mission, 0, u16(0)),
        34,
        "RecordType zero is an invalid field value, not an unknown extension record.",
    )
    add(
        "unsupported_record_version",
        "record_unsupported_version",
        2,
        replaced(mission, 2, u8(2)),
        27,
        "The required record uses an unsupported record_version.",
    )
    add(
        "reserved_record_flag",
        "record_reserved_flag",
        2,
        replaced(mission, 3, u8(0x80)),
        36,
        "A v1 state record sets a reserved record flag.",
    )

    # CLASS_MANIFEST prefix: envelope(6), generation(4), class_id(4), presence(8), utf8 length(2).
    add(
        "invalid_utf8_string",
        "invalid_utf8",
        3,
        replaced(replaced(class_manifest, 22, u16(1)), 24, b"\xff"),
        31,
        "The internal name begins with an invalid UTF-8 octet.",
    )
    add(
        "string_over_field_limit",
        "string_too_long",
        3,
        replaced(class_manifest, 22, u16(256)),
        32,
        "The internal-name byte length exceeds the 255-octet field limit.",
    )

    # MISSION_STATE binary32 begins at envelope + presence/generation/enums/reserved.
    float_offset = 6 + 8 + 4 + 1 + 1 + 2
    for name, bits in (
        ("nan_float", 0x7FC00000),
        ("positive_infinity_float", 0x7F800000),
        ("negative_infinity_float", 0xFF800000),
    ):
        add(
            name,
            "non_finite_float",
            2,
            replaced(mission, float_offset, u32(bits)),
            33,
            "A binary32 field contains a non-finite IEEE-754 value.",
        )
    add(
        "bool_outside_closed_domain",
        "bool_outside_0_1",
        2,
        replaced(mission, 6 + 8 + 4 + 1, u8(2)),
        34,
        "The paused bool8 value is outside {0,1}.",
    )
    add(
        "closed_enum_unknown",
        "closed_enum_unknown",
        20,
        replaced(cargo, 6 + 8 + 8 + 8, u8(0xFF)),
        35,
        "Cargo scan phase is an unknown value of a closed enum.",
    )

    quaternion_offset = 6 + 8 + 8 + 8 + 12
    add(
        "quaternion_non_finite",
        "quaternion_non_finite",
        7,
        replaced(flight, quaternion_offset, u32(0x7FC00000)),
        33,
        "The quaternion scalar component is NaN.",
    )
    add(
        "quaternion_zero_non_normalizable",
        "quaternion_non_normalizable",
        7,
        replaced(flight, quaternion_offset, f32(0.0)),
        34,
        "The all-zero quaternion cannot be normalized.",
    )
    add(
        "quaternion_non_canonical_sign",
        "quaternion_non_canonical",
        7,
        replaced(flight, quaternion_offset, f32(-1.0)),
        34,
        "A normalized quaternion uses the forbidden negative canonical sign.",
    )

    # Duplicate the canonical COMM_ASSET_MANIFEST entry while keeping framing exact.
    payload = bytearray(comm_manifest[6:])
    converter_bytes = payload[64 : 64 + payload[36] + payload[37]]
    entry_offset = 64 + len(converter_bytes)
    entry = bytes(payload[entry_offset:])
    payload[54:58] = u32(2)
    payload[62:64] = u16(2)
    duplicate_manifest = with_record_payload(25, bytes(payload) + entry)
    add(
        "duplicate_comm_asset_id",
        "record_duplicate_item_key",
        25,
        duplicate_manifest,
        30,
        "Two manifest entries expose the same canonical asset_id item key.",
    )

    quota_payload = bytearray(comm_manifest[6:])
    quota_payload[54:58] = u32(4097)
    quota_payload[62:64] = u16(4097)
    add(
        "comm_asset_count_over_quota",
        "quota_before_allocation",
        25,
        with_record_payload(25, bytes(quota_payload)),
        46,
        "The 4096-entry manifest quota is rejected before entry iteration.",
    )
    add(
        "comm_bundle_version_inconsistent",
        "comm_bundle_inconsistent",
        25,
        replaced(comm_manifest, 6, u16(2)),
        34,
        "The manifest advertises a non-v1 communication bundle version.",
    )
    add(
        "comm_bundle_hash_mismatch",
        "comm_bundle_hash_inconsistent",
        25,
        comm_manifest,
        44,
        "The selected bundle hash differs from the manifest bundle hash.",
        context={"expectedBundleHash": "01" * 32},
    )
    add(
        "comm_duration_mismatch",
        "comm_duration_inconsistent",
        27,
        comm_event,
        44,
        "The START duration differs from the installed asset manifest.",
        context={"assetDurationsUs": {records[27][2]["fields"]["head_asset_id"]: "200000"}},
    )
    return fixtures


def build_invalid_messages(
    records: dict[int, tuple[str, bytes, dict[str, Any]]],
    messages: dict[int, tuple[str, int, bytes, dict[str, Any], dict[str, Any]]],
) -> list[dict[str, Any]]:
    fixtures: list[dict[str, Any]] = []

    def add(
        name: str,
        category: str,
        message_type: int,
        payload: bytes,
        error: int,
        notes: str,
        *,
        flags: int | None = None,
        context: dict[str, Any] | None = None,
    ) -> None:
        if error not in VALIDATION_ERROR_NAMES:
            raise RuntimeError(f"unregistered fixture ValidationError {error}")
        fixtures.append(
            {
                "category": category,
                "context": context or {},
                "encoded": payload,
                "error": error,
                "kind": "message-payload",
                "messageFlags": messages.get(message_type, ("", 0, b"", {}, {}))[1] if flags is None else flags,
                "messageType": message_type,
                "name": name,
                "notes": notes,
            }
        )

    heartbeat = messages[9][2]
    add(
        "fixed_payload_truncated",
        "fixed_payload_truncated",
        9,
        heartbeat[:-1],
        24,
        "A fixed-layout HEARTBEAT is one byte short.",
    )
    add(
        "fixed_payload_trailing_byte",
        "fixed_payload_trailing",
        9,
        heartbeat + b"\x00",
        25,
        "A fixed-layout HEARTBEAT contains one trailing byte.",
    )
    add(
        "unknown_message_type",
        "client_undefined_command",
        255,
        b"",
        11,
        "A client cannot invent an undefined command MessageType.",
        flags=0,
        context={"senderRole": "client"},
    )
    add(
        "client_simulated_producer_command",
        "client_simulated_command",
        4,
        messages[4][2],
        12,
        "A client sends the producer-only SESSION_BEGIN shape as a command.",
        context={"allowedSenderRoles": ["producer"], "senderRole": "client"},
    )

    # Rebuild a snapshot with two singleton SESSION_STATE records and a matching hash.
    singleton_region = records[1][1] + records[1][1]
    duplicate_snapshot = (
        u32(2)
        + u16(0)
        + u16(1)
        + u32(len(singleton_region))
        + hashlib.sha256(singleton_region).digest()
        + u64(SAMPLE_TIME_US)
        + u32(1)
        + u16(1)
        + u16(2)
        + singleton_region
    )
    add(
        "duplicate_singleton_record",
        "record_duplicate_singleton",
        6,
        duplicate_snapshot,
        29,
        "A FULL_SNAPSHOT contains SESSION_STATE twice.",
    )

    quota_batch = replaced(messages[8][2], 22, u16(0xFFFF))
    add(
        "event_batch_record_count_exceeds_region",
        "record_count_truncated_region",
        8,
        quota_batch,
        24,
        "record_count is legal u16 but exceeds the records present in the payload.",
    )

    unknown_baseline = replaced(messages[7][2], 0, u32(99))
    add(
        "delta_unknown_baseline",
        "unknown_baseline",
        7,
        unknown_baseline,
        39,
        "A totally unknown baseline is discarded and requests resynchronization; only a known candidate may wait.",
        context={"knownCandidateBaselines": [2], "knownCommittedBaselines": [1]},
    )
    add(
        "snapshot_missing_manifest",
        "unknown_manifest",
        6,
        replaced(messages[6][2], 52, u32(99)),
        41,
        "The snapshot references a manifest that is not installed.",
        context={"installedManifestIds": [1]},
    )

    frame = messages[16][2]
    frame_context = {"activeVideoStreams": [1], "currentVideoGeneration": {"1": 1}, "knownEntityIds": [2]}
    add(
        "video_stale_generation",
        "unknown_generation",
        16,
        replaced(frame, 4, u32(2)),
        40,
        "The frame config_generation does not match the active stream generation.",
        context=frame_context,
    )
    add(
        "video_unknown_stream",
        "unknown_stream",
        16,
        replaced(frame, 0, u32(99)),
        44,
        "The frame references a stream that is not active.",
        context={"activeVideoStreams": [1]},
    )
    add(
        "video_unknown_target",
        "unknown_target",
        16,
        replaced(frame, 12, u64(99)),
        38,
        "The frame references an entity unknown to the client state image.",
        context={"knownEntityIds": [2]},
    )
    add(
        "video_old_target_after_change",
        "stale_video_target",
        16,
        frame,
        40,
        "A late frame retains the generation and entity selected before the stream target change.",
        context={
            "activeVideoStreams": [1],
            "currentVideoGeneration": {"1": 2},
            "currentVideoTarget": {"1": "3"},
            "knownEntityIds": [2, 3],
        },
    )
    add(
        "video_encoded_frame_size_mismatch",
        "encoded_frame_size_mismatch",
        16,
        replaced(frame, 28, u32(len(frame) - 36 + 1)),
        24,
        "encoded_frame_size declares one byte more than the remaining access unit.",
    )

    ack_context = {
        "endpoint": "client-a",
        "reliableTarget": {
            "crc32": "0x12345678",
            "endpoint": "client-a",
            "fragmentCount": 1,
            "messageId": 100,
            "messageType": 6,
            "pending": True,
        },
    }
    add(
        "ack_forged_target",
        "ack_forged",
        10,
        replaced(messages[10][2], 0, u32(101)),
        44,
        "The ACK target tuple was never emitted by the producer.",
        context=ack_context,
    )
    late_context = json.loads(json.dumps(ack_context))
    late_context["reliableTarget"]["pending"] = False
    add(
        "ack_late_after_retention",
        "ack_late",
        10,
        messages[10][2],
        44,
        "The ACK arrives after the target left the reliable retention window.",
        context=late_context,
    )
    endpoint_context = json.loads(json.dumps(ack_context))
    endpoint_context["endpoint"] = "client-b"
    add(
        "ack_wrong_endpoint",
        "ack_wrong_endpoint",
        10,
        messages[10][2],
        14,
        "The ACK arrives from an endpoint different from the retained target owner.",
        context=endpoint_context,
    )
    add(
        "ack_wrong_target_crc",
        "ack_wrong_crc",
        10,
        replaced(messages[10][2], 8, u32(0xAABBCCDD)),
        44,
        "The ACK target CRC differs from the retained logical message CRC.",
        context=ack_context,
    )
    add(
        "nack_bitmap_tail_bits_set",
        "nack_bitmap_incoherent",
        11,
        replaced(messages[11][2], len(messages[11][2]) - 1, b"\xfe"),
        36,
        "A nine-fragment NACK sets reserved tail bits in its second bitmap byte.",
    )

    negotiation = {
        "videoNegotiation": {
            "codecProfiles": [100],
            "codecs": [1],
            "maxBitrateKbps": 4000,
            "maxHeight": 1024,
            "maxWidth": 1024,
        }
    }
    config = messages[15][2]
    add(
        "video_codec_not_negotiated",
        "video_codec_not_negotiated",
        15,
        config,
        42,
        "The valid H.264 codec was not negotiated at the capability layer.",
        context={
            "videoNegotiation": {
                **negotiation["videoNegotiation"],
                "codecs": [],
            }
        },
    )
    add(
        "video_profile_not_negotiated",
        "video_profile_not_negotiated",
        15,
        replaced(config, 6, u8(77)),
        42,
        "The selected codec profile was not offered by the subscriber.",
        context=negotiation,
    )
    add(
        "video_resolution_not_negotiated",
        "video_resolution_not_negotiated",
        15,
        config,
        42,
        "The selected width exceeds the negotiated maximum.",
        context={
            "videoNegotiation": {
                **negotiation["videoNegotiation"],
                "maxWidth": 958,
            }
        },
    )
    add(
        "video_bitrate_not_negotiated",
        "video_bitrate_not_negotiated",
        15,
        replaced(config, 28, u32(4001)),
        42,
        "The selected bitrate exceeds the negotiated maximum.",
        context=negotiation,
    )
    return fixtures


def fixture_evidence(path: str) -> dict[str, str]:
    return {"kind": "fixture", "path": path}


def test_evidence(path: str, suite: str, name: str) -> dict[str, str]:
    return {"kind": "cpp-test", "name": name, "path": path, "suite": suite}


def invalid_fixture_metadata_path(fixture: dict[str, Any]) -> str:
    kind_directory = "messages" if fixture["kind"] == "message-payload" else "records"
    return f"vectors/invalid/{kind_directory}/{fixture['category']}/{fixture['name']}.json"


def build_protocol_coverage(
    records: dict[int, tuple[str, bytes, dict[str, Any]]],
    messages: dict[int, tuple[str, int, bytes, dict[str, Any], dict[str, Any]]],
    invalid_records: list[dict[str, Any]],
    invalid_messages: list[dict[str, Any]],
) -> dict[str, Any]:
    def message_fixture(message_type: int) -> dict[str, str]:
        slug = messages[message_type][0].lower()
        return fixture_evidence(f"vectors/valid/messages/{slug}/{slug}.json")

    def record_fixture(record_type: int) -> dict[str, str]:
        slug = records[record_type][0].lower()
        return fixture_evidence(f"vectors/valid/records/{slug}/{slug}.json")

    def invalid_transport_fixture(name: str) -> dict[str, str]:
        return fixture_evidence(f"vectors/invalid/datagrams/transport/{name}/{name}.json")

    invalid = invalid_records + invalid_messages

    def invalid_categories(*categories: str) -> list[dict[str, str]]:
        wanted = set(categories)
        return [
            fixture_evidence(invalid_fixture_metadata_path(fixture))
            for fixture in invalid
            if fixture["category"] in wanted
        ]

    packet_io = "test/src/telemetry/protocol/test_packet_io.cpp"
    constants = "test/src/telemetry/protocol/test_protocol_constants.cpp"
    control = "test/src/telemetry/protocol/test_telemetry_control_messages.cpp"
    reliability = "test/src/telemetry/protocol/test_telemetry_reliability_messages.cpp"
    state = "test/src/telemetry/protocol/test_telemetry_state_messages.cpp"
    replication = "test/src/telemetry/protocol/test_telemetry_replication.cpp"
    replication_harness = "test/src/telemetry/protocol/test_telemetry_replication_harness.cpp"
    specialized = "test/src/telemetry/protocol/test_telemetry_specialized_views.cpp"
    lifecycle = "test/src/telemetry/protocol/test_telemetry_specialized_lifecycle.cpp"
    datagram = "test/src/telemetry/protocol/test_telemetry_datagram.cpp"
    fragmentation = "test/src/telemetry/protocol/test_telemetry_fragmentation.cpp"
    counters = "test/src/telemetry/protocol/test_telemetry_counters.cpp"
    business_1_10 = "test/src/telemetry/protocol/test_telemetry_business_records_1_10.cpp"

    valid_catalogue = [
        {
            "id": "P0-CAT-VALID-01",
            "clause": "each scalar, string, bytes and list",
            "evidence": [
                test_evidence(packet_io, "TelemetryProtocolPacketIo", "IntegerExtremaUseExactWidthsAndRejectShortBuffers"),
                test_evidence(packet_io, "TelemetryProtocolPacketIo", "ByteStringsRoundTripAndHonorExactCapacity"),
                test_evidence(packet_io, "TelemetryProtocolPacketIo", "Utf8RoundTripUsesByteLength"),
                test_evidence(state, "TelemetryProtocolStateMessages", "ManifestGoldenBytesRoundTripAndAliasedEncoding"),
            ],
        },
        {
            "id": "P0-CAT-VALID-02",
            "clause": "empty logical payload, valid unfragmented and valid multi-fragment messages",
            "evidence": [
                fixture_evidence("vectors/valid/datagrams/transport/transport_0_bytes/transport_0_bytes.json"),
                fixture_evidence("vectors/valid/datagrams/transport/transport_1_bytes/transport_1_bytes.json"),
                fixture_evidence("vectors/valid/datagrams/transport/transport_1133_bytes/transport_1133_bytes.json"),
                test_evidence(fragmentation, "TelemetryProtocolFragmenter", "CanonicalSlicesCoverEmptyExactAndFinalRemainderBoundaries"),
            ],
        },
        {
            "id": "P0-CAT-VALID-03",
            "clause": "all 20 v1 MessageType payloads",
            "evidence": [message_fixture(value) for value in sorted(messages)],
        },
        {
            "id": "P0-CAT-VALID-04",
            "clause": "all 28 v1 RecordType envelopes",
            "evidence": [record_fixture(value) for value in sorted(records)],
        },
        {
            "id": "P0-CAT-VALID-05",
            "clause": "every enum and every flag/capability bit",
            "evidence": [
                test_evidence(constants, "TelemetryProtocolConstants", "FreezesMessageFlagsAndPerClassLimits"),
                test_evidence(constants, "TelemetryProtocolConstants", "FreezesCapabilitiesAndControlRegistries"),
                test_evidence(constants, "TelemetryProtocolConstants", "FreezesValidationErrorRegistry"),
            ],
        },
        {
            "id": "P0-CAT-VALID-06",
            "clause": "HELLO accepted/refused, WELCOME and complete heartbeat exchange",
            "evidence": [
                message_fixture(2),
                message_fixture(3),
                message_fixture(9),
                test_evidence(control, "TelemetryProtocolControlMessages", "WelcomeAcceptedAndRejectedFormsRoundTripAndRejectContradictions"),
                test_evidence(control, "TelemetryProtocolControlMessages", "HeartbeatEnforcesKindReservedBytesTimestampsAndExactSize"),
            ],
        },
        {
            "id": "P0-CAT-VALID-07",
            "clause": "ACK VALIDATED, ACK APPLIED and NACK bitmap",
            "evidence": [
                message_fixture(10),
                message_fixture(11),
                test_evidence(reliability, "TelemetryProtocolReliabilityMessages", "AckGoldenBytesRoundTripAndExactLength"),
                test_evidence(reliability, "TelemetryProtocolReliabilityMessages", "MissingBitmapBoundariesTailBitsAndMaxAreExact"),
            ],
        },
        {
            "id": "P0-CAT-VALID-08",
            "clause": "manifest, initial snapshot, candidate keyframe and cumulative delta",
            "evidence": [
                message_fixture(5),
                message_fixture(6),
                message_fixture(7),
                test_evidence(replication, "TelemetryProtocolReplication", "ProducerPromotesOnlyAfterEveryExactAppliedAck"),
            ],
        },
        {
            "id": "P0-CAT-VALID-09",
            "clause": "lost intermediate delta followed by a newer cumulative delta",
            "evidence": [
                test_evidence(replication, "TelemetryProtocolReplication", "LatestCumulativeDeltaConvergesAfterEveryIntermediateLoss"),
                test_evidence(replication_harness, "TelemetryProtocolReplicationHarness", "ReorderedSnapshotLostAckAndLostDeltaStillConverge"),
            ],
        },
        {
            "id": "P0-CAT-VALID-10",
            "clause": "baseline change with mutation during ACK round trip",
            "evidence": [
                test_evidence(replication, "TelemetryProtocolReplication", "ProducerKeepsBothDirtySetsAndPreservesPostCaptureMutations"),
            ],
        },
        {
            "id": "P0-CAT-VALID-11",
            "clause": "communication manifest, state and START/STOP events",
            "evidence": [
                record_fixture(25),
                record_fixture(26),
                record_fixture(27),
                test_evidence(specialized, "TelemetryProtocolSpecializedViews", "CommStateAndEventsEnforceCanonicalActiveAndStopForms"),
            ],
        },
        {
            "id": "P0-CAT-VALID-12",
            "clause": "all TARGET_VIDEO messages including fragmented IDR",
            "evidence": [
                *[message_fixture(value) for value in range(14, 20)],
                test_evidence(datagram, "TelemetryProtocolDatagram", "KeyframeAndVideoIdrFlagsAreTypeSpecificAndMutuallyExclusive"),
                test_evidence(lifecycle, "TelemetryProtocolSpecializedLifecycle", "ProducerRequiresWelcomeConfigAckAndFirstIdr"),
            ],
        },
        {
            "id": "P0-CAT-VALID-13",
            "clause": "serial arithmetic near counter wrap",
            "evidence": [
                test_evidence(counters, "TelemetryProtocolCounters", "SerialU32OrderingHandlesWrapAndExactHalfRange"),
                test_evidence(counters, "TelemetryProtocolCounters", "ProbeIdsSkipZeroAtWrapAndSessionResetPurgesOldSlots"),
            ],
        },
        {
            "id": "P0-CAT-VALID-14",
            "clause": "UTF-8, quaternion and business min/max boundaries",
            "evidence": [
                test_evidence(packet_io, "TelemetryProtocolPacketIo", "Utf8RoundTripUsesByteLength"),
                test_evidence(business_1_10, "TelemetryProtocolBusinessRecords1To10", "FlightStateEnforcesCanonicalQuaternionFiniteRangesAndFlags"),
                test_evidence(specialized, "TelemetryProtocolSpecializedViews", "AvcLimitsCoverMaxFsMaxMbpsAndMaxBrBoundaries"),
            ],
        },
    ]

    invalid_transport_catalogue = [
        {
            "id": "P0-CAT-INVALID-01",
            "clause": "every possible truncation length of the fixed datagram header",
            "evidence": [invalid_transport_fixture(f"truncated_header_{size:02d}") for size in range(68)],
        },
        {
            "id": "P0-CAT-INVALID-02",
            "clause": "bad magic, major/minor version, header size and reserved header flags",
            "evidence": [
                invalid_transport_fixture(name)
                for name in (
                    "bad_magic",
                    "unsupported_major",
                    "unsupported_minor",
                    "bad_header_size",
                    "reserved_header_flag",
                )
            ],
        },
        {
            "id": "P0-CAT-INVALID-03",
            "clause": "actual datagram length differs from declared length",
            "evidence": [invalid_transport_fixture("trailing_datagram_byte")],
        },
        {
            "id": "P0-CAT-INVALID-04",
            "clause": "bad datagram CRC and bad logical message CRC",
            "evidence": [invalid_transport_fixture("bad_datagram_crc"), invalid_transport_fixture("bad_message_crc")],
        },
        {
            "id": "P0-CAT-INVALID-05",
            "clause": "zero fragment count, out-of-range index, non-canonical offset and overflowing slice",
            "evidence": [
                invalid_transport_fixture(name)
                for name in (
                    "zero_fragment_count",
                    "fragment_index_out_of_range",
                    "fragment_offset_noncanonical",
                    "fragment_slice_noncanonical",
                )
            ],
        },
        {
            "id": "P0-CAT-INVALID-06",
            "clause": "inconsistent, overlapping and contradictory duplicate fragments",
            "evidence": [
                invalid_transport_fixture("inconsistent_fragment_metadata"),
                invalid_transport_fixture("overlapping_fragment"),
                invalid_transport_fixture("contradictory_duplicate_fragment"),
            ],
        },
        {
            "id": "P0-CAT-INVALID-07",
            "clause": "message and list beyond normative maxima",
            "evidence": [
                invalid_transport_fixture("message_too_large"),
                *invalid_categories("quota_before_allocation"),
            ],
        },
        {
            "id": "P0-CAT-INVALID-08",
            "clause": "resource quotas rejected before allocation",
            "evidence": [
                invalid_transport_fixture("reassembly_quota_before_allocation"),
                *invalid_categories("quota_before_allocation"),
            ],
        },
    ]
    invalid_groups = [
        ("P0-CAT-INVALID-09", "fixed payload truncation and trailing bytes", ("fixed_payload_truncated", "fixed_payload_trailing")),
        ("P0-CAT-INVALID-10", "record truncation, excessive length, duplicate singleton and item key", ("record_truncated", "record_length_excessive", "record_duplicate_singleton", "record_duplicate_item_key")),
        ("P0-CAT-INVALID-11", "invalid UTF-8 and oversized string", ("invalid_utf8", "string_too_long")),
        ("P0-CAT-INVALID-12", "NaN, infinities, bool8 and closed enum", ("non_finite_float", "bool_outside_0_1", "closed_enum_unknown")),
        ("P0-CAT-INVALID-13", "non-finite, zero/non-normalizable and non-canonical quaternion", ("quaternion_non_finite", "quaternion_non_normalizable", "quaternion_non_canonical")),
        ("P0-CAT-INVALID-14", "unknown baseline, generation, stream, target and manifest", ("unknown_baseline", "unknown_generation", "unknown_stream", "unknown_target", "unknown_manifest")),
        ("P0-CAT-INVALID-15", "forged, late, wrong-endpoint, wrong-CRC or bitmap-incoherent ACK/NACK", ("ack_forged", "ack_late", "ack_wrong_endpoint", "ack_wrong_crc", "nack_bitmap_incoherent")),
        ("P0-CAT-INVALID-16", "inconsistent communication bundle, hash and duration", ("comm_bundle_inconsistent", "comm_bundle_hash_inconsistent", "comm_duration_inconsistent")),
        ("P0-CAT-INVALID-17", "video codec/profile/resolution/bitrate not negotiated", ("video_codec_not_negotiated", "video_profile_not_negotiated", "video_resolution_not_negotiated", "video_bitrate_not_negotiated")),
        ("P0-CAT-INVALID-18", "encoded_frame_size mismatch", ("encoded_frame_size_mismatch",)),
        ("P0-CAT-INVALID-19", "old frame after target change", ("stale_video_target",)),
        ("P0-CAT-INVALID-20", "client simulated or undefined command", ("client_simulated_command", "client_undefined_command")),
    ]
    invalid_catalogue = invalid_transport_catalogue + [
        {"id": item_id, "clause": clause, "evidence": invalid_categories(*categories)}
        for item_id, clause, categories in invalid_groups
    ]
    return {
        "generatedBy": "tools/generate_protocol_vectors.py",
        "invalidCatalogue": invalid_catalogue,
        "invalidCategoryFixtureMap": {
            category: [
                invalid_fixture_metadata_path(fixture)
                for fixture in invalid
                if fixture["category"] == category
            ]
            for category in sorted({fixture["category"] for fixture in invalid})
        },
        "schema": f"{SCHEMA}-catalogue-coverage",
        "sourceSections": [
            "documentation/analysis/specs/0-Contrat-de-protocole/06-validation-securite-et-conformite.md#104-catalogue-minimal-valide",
            "documentation/analysis/specs/0-Contrat-de-protocole/06-validation-securite-et-conformite.md#105-catalogue-minimal-invalide",
        ],
        "validCatalogue": valid_catalogue,
    }


def encoded_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def metadata(
    *,
    name: str,
    kind: str,
    input_name: str,
    expected_path: str,
    message_type: int | None = None,
    message_flags: int | None = None,
    record_type: int | None = None,
    record_flags: int | None = None,
    context: dict[str, Any] | None = None,
) -> bytes:
    value: dict[str, Any] = {
        "expectedCanonicalJson": expected_path,
        "expectedValidationError": 0,
        "expectedValidationErrorName": "NONE",
        "inputFiles": [input_name],
        "kind": kind,
        "name": name,
        "notes": "Deterministic Phase 0 canonical fixture; bytes are authoritative.",
        "schema": SCHEMA,
        "valid": True,
    }
    if message_type is not None:
        value["messageType"] = message_type
        value["messageFlags"] = message_flags or 0
    if record_type is not None:
        value["recordType"] = record_type
        value["recordVersion"] = 1
        value["recordFlags"] = record_flags or 0
    if context:
        value["context"] = context
    return encoded_json(value)


def cxx_replay_metadata(fixture: dict[str, Any]) -> dict[str, Any]:
    name = fixture["name"]
    if fixture["kind"] == "record":
        record_context_apis = {
            "comm_bundle_hash_mismatch": [
                "RecordEnvelopeIterator::next",
                "decode_comm_asset_manifest_payload",
                "CommViewClientLifecycle::install_bundle",
                "comm_view_lifecycle_validation_error",
            ],
            "comm_duration_mismatch": [
                "RecordEnvelopeIterator::next",
                "decode_comm_view_event_payload",
                "CommViewClientLifecycle::apply_event",
                "comm_view_lifecycle_validation_error",
            ],
        }
        if name in record_context_apis:
            return {"api": record_context_apis[name], "mode": "semantic-context", "status": "exact"}
        return {
            "api": ["RecordEnvelopeIterator::next", "validate_business_record"],
            "mode": "record-envelope",
            "status": "exact",
        }

    exact_payload_apis = {
        "duplicate_singleton_record": ["decode_full_snapshot_part_payload", "decode_business_snapshot_region"],
        "event_batch_record_count_exceeds_region": ["decode_event_batch_payload"],
        "fixed_payload_trailing_byte": ["decode_heartbeat_payload"],
        "fixed_payload_truncated": ["decode_heartbeat_payload"],
        "nack_bitmap_tail_bits_set": ["decode_nack_payload"],
        "video_encoded_frame_size_mismatch": ["decode_target_video_frame_payload"],
    }
    if name in exact_payload_apis:
        return {"api": exact_payload_apis[name], "mode": "payload", "status": "exact"}

    exact_context_apis = {
        "ack_forged_target": ["decode_ack_payload", "validate_ack_target"],
        "ack_wrong_endpoint": ["decode_ack_payload", "validate_received_datagram_context"],
        "ack_wrong_target_crc": ["decode_ack_payload", "validate_ack_target"],
        "client_simulated_producer_command": ["validate_received_datagram_context"],
        "unknown_message_type": ["validate_received_datagram_context"],
    }
    if name in exact_context_apis:
        return {"api": exact_context_apis[name], "mode": "stateful-context", "status": "exact"}

    semantic_context_apis = {
        "ack_late_after_retention": [
            "decode_ack_payload",
            "ReliableSendWindow::acknowledge",
            "reliable_response_validation_error",
        ],
        "delta_unknown_baseline": [
            "decode_delta_payload",
            "ClientReplicationModel::receive_delta",
            "client_delta_validation_error",
        ],
        "snapshot_missing_manifest": [
            "decode_full_snapshot_part_payload",
            "ClientReplicationModel::commit_snapshot",
            "snapshot_commit_validation_error",
        ],
        "video_stale_generation": [
            "decode_target_video_frame_payload",
            "TargetVideoClientLifecycle::accept_frame",
            "video_lifecycle_validation_error",
        ],
        "video_unknown_stream": [
            "decode_target_video_frame_payload",
            "TargetVideoClientLifecycle::accept_frame",
            "video_lifecycle_validation_error",
        ],
        "video_unknown_target": [
            "decode_target_video_frame_payload",
            "validate_target_video_frame_entity_context",
        ],
        "video_old_target_after_change": [
            "decode_target_video_frame_payload",
            "TargetVideoClientLifecycle::accept_frame",
            "video_lifecycle_validation_error",
        ],
        "video_bitrate_not_negotiated": ["validate_target_video_config_for_subscribe"],
        "video_codec_not_negotiated": ["validate_target_video_config_for_subscribe"],
        "video_profile_not_negotiated": ["validate_target_video_config_for_subscribe"],
        "video_resolution_not_negotiated": ["validate_target_video_config_for_subscribe"],
    }
    return {"api": semantic_context_apis[name], "mode": "semantic-context", "status": "exact"}


def invalid_metadata(fixture: dict[str, Any], input_name: str) -> bytes:
    value: dict[str, Any] = {
        "expectedValidationError": fixture["error"],
        "expectedValidationErrorName": VALIDATION_ERROR_NAMES[fixture["error"]],
        "inputFiles": [input_name],
        "invalidCategory": fixture["category"],
        "kind": fixture["kind"],
        "name": fixture["name"],
        "notes": fixture["notes"],
        "requirementIds": ["P0-REQ-205", "P0-AC-019"],
        "schema": SCHEMA,
        "valid": False,
    }
    value["cxxReplay"] = cxx_replay_metadata(fixture)
    if fixture["kind"] == "message-payload":
        value["messageType"] = fixture["messageType"]
        value["messageFlags"] = fixture["messageFlags"]
    else:
        value["recordType"] = fixture["recordType"]
        value["recordVersion"] = fixture["recordVersion"]
        value["recordFlags"] = fixture["recordFlags"]
    if fixture["context"]:
        value["context"] = fixture["context"]
    return encoded_json(value)


def build_files() -> dict[str, bytes]:
    records = build_records()
    messages = build_messages(records)
    invalid_records = build_invalid_records(records)
    invalid_messages = build_invalid_messages(records, messages)
    files: dict[str, bytes] = {}

    for record_type, (symbol, encoded, expected) in records.items():
        slug = symbol.lower()
        base = f"vectors/valid/records/{slug}/{slug}"
        expected_path = f"expected/records/{slug}.json"
        files[f"{base}.bin"] = encoded
        files[f"{base}.json"] = metadata(
            name=slug,
            kind="record",
            input_name=f"{slug}.bin",
            expected_path=f"records/{slug}.json",
            record_type=record_type,
            record_flags=expected["recordFlags"],
            context={
                "recordContainer": (
                    "manifest"
                    if record_type in (3, 4, 25)
                    else "event-batch-reliable"
                    if record_type in (27, 28)
                    else "full-snapshot"
                )
            },
        )
        files[expected_path] = encoded_json(expected)

    for message_type, (symbol, flags, payload, expected, context) in messages.items():
        slug = symbol.lower()
        base = f"vectors/valid/messages/{slug}/{slug}"
        expected_path = f"expected/messages/{slug}.json"
        files[f"{base}.bin"] = payload
        files[f"{base}.json"] = metadata(
            name=slug,
            kind="message-payload",
            input_name=f"{slug}.bin",
            expected_path=f"messages/{slug}.json",
            message_type=message_type,
            message_flags=flags,
            context=context,
        )
        files[expected_path] = encoded_json(expected)

    for fixture in invalid_records + invalid_messages:
        category = fixture["category"]
        name = fixture["name"]
        kind_directory = "messages" if fixture["kind"] == "message-payload" else "records"
        base = f"vectors/invalid/{kind_directory}/{category}/{name}"
        input_name = f"{name}.bin"
        files[f"{base}.bin"] = fixture["encoded"]
        files[f"{base}.json"] = invalid_metadata(fixture, input_name)

    files["protocol-coverage.json"] = encoded_json(
        build_protocol_coverage(records, messages, invalid_records, invalid_messages)
    )
    manifest_entries = [
        {"path": relative, "sha256": hashlib.sha256(contents).hexdigest()}
        for relative, contents in sorted(files.items())
    ]
    manifest = {
        "decoder": "tools/fstl_reference_decoder.py",
        "files": manifest_entries,
        "generator": "tools/generate_protocol_vectors.py",
        "invalidMessageFixtureCount": len(invalid_messages),
        "invalidRecordFixtureCount": len(invalid_records),
        "messageTypes": list(range(1, 21)),
        "recordTypes": list(range(1, 29)),
        "schema": SCHEMA,
    }
    files["protocol-vectors.manifest.json"] = encoded_json(manifest)
    return files


def owned_directories(root: Path) -> tuple[Path, ...]:
    return (
        root / "vectors/valid/messages",
        root / "vectors/valid/records",
        root / "vectors/invalid/messages",
        root / "vectors/invalid/records",
        root / "expected/messages",
        root / "expected/records",
    )


def compare(root: Path, expected: dict[str, bytes]) -> list[str]:
    differences: list[str] = []
    for relative, contents in expected.items():
        path = root / relative
        if not path.exists():
            differences.append(f"missing: {relative}")
        elif path.read_bytes() != contents:
            differences.append(f"different: {relative}")
    actual = {
        path.relative_to(root).as_posix()
        for directory in owned_directories(root)
        if directory.exists()
        for path in directory.rglob("*")
        if path.is_file()
    }
    actual.add("protocol-vectors.manifest.json")
    actual.add("protocol-coverage.json")
    for relative in sorted(actual - set(expected)):
        differences.append(f"unexpected: {relative}")
    return differences


def remove_previous(root: Path) -> None:
    manifest_path = root / "protocol-vectors.manifest.json"
    if not manifest_path.exists():
        return
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("generator") != "tools/generate_protocol_vectors.py":
        raise RuntimeError("refusing to remove files from an unrecognized manifest")
    allowed_roots = tuple(path.resolve() for path in owned_directories(root))
    allowed_files = {(root / "protocol-coverage.json").resolve()}
    for entry in manifest.get("files", []):
        relative = entry.get("path") if isinstance(entry, dict) else None
        if not isinstance(relative, str):
            raise RuntimeError("invalid protocol vector manifest entry")
        candidate = (root / relative).resolve()
        if candidate not in allowed_files and not any(candidate.is_relative_to(allowed) for allowed in allowed_roots):
            raise RuntimeError("protocol vector manifest path escapes owned directories")
        if candidate.is_file():
            candidate.unlink()
    manifest_path.unlink()


def materialize(root: Path, files: dict[str, bytes]) -> None:
    for relative, contents in files.items():
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents)


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    files = build_files()
    if args.write:
        remove_previous(root)
        materialize(root, files)
        print(f"wrote {len(files)} protocol vector files")
        return 0

    differences = compare(root, files)
    if differences:
        print("\n".join(differences))
        return 1
    print("verified 20 MessageType and 28 RecordType golden vectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

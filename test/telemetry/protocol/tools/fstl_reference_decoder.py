#!/usr/bin/env python3
"""Independent standard-library decoder for the FSTL 1.0 golden fixtures.

The decoder intentionally imports neither the C++ implementation nor the
fixture generator.  It uses a second, bitwise CRC-32/ISO-HDLC calculation and
explicit little-endian reads so the fixture check is meaningful on every host
endianness.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any


SCHEMA = "FSTL-1.0"
HEADER_SIZE = 68
MAX_FRAGMENT_PAYLOAD = 1132

MESSAGE_NAMES = {
    1: "DISCOVERY",
    2: "HELLO",
    3: "WELCOME",
    4: "SESSION_BEGIN",
    5: "MANIFEST",
    6: "FULL_SNAPSHOT",
    7: "DELTA",
    8: "EVENT_BATCH",
    9: "HEARTBEAT",
    10: "ACK",
    11: "NACK",
    12: "RESYNC_REQUEST",
    13: "SESSION_END",
    14: "TARGET_VIDEO_SUBSCRIBE",
    15: "TARGET_VIDEO_CONFIG",
    16: "TARGET_VIDEO_FRAME",
    17: "TARGET_VIDEO_KEYFRAME_REQUEST",
    18: "TARGET_VIDEO_STOP",
    19: "TARGET_VIDEO_STATS",
    20: "CAPABILITY_UPDATE",
}

RECORD_NAMES = {
    1: "SESSION_STATE",
    2: "MISSION_STATE",
    3: "CLASS_MANIFEST",
    4: "WEAPON_MANIFEST",
    5: "ENTITY_LIFECYCLE",
    6: "SHIP_IDENTITY",
    7: "FLIGHT_STATE",
    8: "CONTROL_STATE",
    9: "DAMAGE_STATE",
    10: "SHIELD_STATE",
    11: "SUBSYSTEM_STATE",
    12: "ENERGY_STATE",
    13: "PROPULSION_STATE",
    14: "WEAPON_STATE",
    15: "LOCK_STATE",
    16: "TARGET_STATE",
    17: "RADAR_STATE",
    18: "RADAR_CONTACTS",
    19: "THREAT_STATE",
    20: "CARGO_SCAN_STATE",
    21: "DOCKING_STATE",
    22: "SUPPORT_STATE",
    23: "NAVIGATION_STATE",
    24: "EFFECT_STATE",
    25: "COMM_ASSET_MANIFEST",
    26: "COMM_VIEW_STATE",
    27: "COMM_VIEW_EVENT",
    28: "EVENTS",
}


class DecodeFailure(Exception):
    def __init__(self, code: int, detail: str):
        super().__init__(detail)
        self.code = code
        self.detail = detail


def fail(code: int, detail: str) -> None:
    raise DecodeFailure(code, detail)


def require(condition: bool, code: int, detail: str) -> None:
    if not condition:
        fail(code, detail)


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    @property
    def remaining(self) -> int:
        return len(self.data) - self.offset

    def take(self, size: int) -> bytes:
        if size < 0 or size > self.remaining:
            fail(24, "truncated payload")
        result = self.data[self.offset : self.offset + size]
        self.offset += size
        return result

    def u8(self) -> int:
        return int.from_bytes(self.take(1), "little", signed=False)

    def u16(self) -> int:
        return int.from_bytes(self.take(2), "little", signed=False)

    def u32(self) -> int:
        return int.from_bytes(self.take(4), "little", signed=False)

    def u64(self) -> int:
        return int.from_bytes(self.take(8), "little", signed=False)

    def i64(self) -> int:
        return int.from_bytes(self.take(8), "little", signed=True)

    def f32(self) -> float:
        import struct

        value = struct.unpack("<f", self.take(4))[0]
        require(math.isfinite(value), 33, "non-finite binary32")
        return 0.0 if value == 0.0 else value

    def utf8(self, limit: int) -> str:
        size = self.u16()
        require(size <= limit, 32, "string exceeds field limit")
        encoded = self.take(size)
        require(b"\x00" not in encoded, 31, "embedded NUL")
        try:
            return encoded.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            fail(31, f"invalid UTF-8: {exc}")

    def finish(self) -> None:
        require(self.remaining == 0, 25, "trailing bytes")


def u64s(value: int) -> str:
    return str(value)


def vec3(reader: Reader) -> list[float]:
    return [reader.f32(), reader.f32(), reader.f32()]


def canonical_record(
    record_type: int,
    version: int,
    flags: int,
    length: int,
    fields: dict[str, Any],
) -> dict[str, Any]:
    return {
        "fields": fields,
        "kind": "record",
        "recordFlags": flags,
        "recordLength": length,
        "recordName": RECORD_NAMES[record_type],
        "recordType": record_type,
        "recordVersion": version,
        "schema": SCHEMA,
    }


def decode_record_payload(record_type: int, reader: Reader) -> dict[str, Any]:
    if record_type == 1:
        presence = reader.u64()
        require(presence in (0, 1), 37, "SESSION_STATE presence")
        producer_id = reader.u64()
        sample = reader.u64()
        authority = reader.u8()
        visibility = reader.u8()
        phase = reader.u8()
        reserved = reader.u8()
        generation = reader.u32()
        capabilities = reader.u64()
        state_coverage = reader.u64()
        derived = reader.u64()
        exact = reader.u64()
        fields = {
            "authority_mode": authority,
            "event_coverage_exact": u64s(exact),
            "event_coverage_state_derived": u64s(derived),
            "negotiated_capabilities": u64s(capabilities),
            "negotiated_capability_generation": generation,
            "presence": u64s(presence),
            "producer_id": u64s(producer_id),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "session_phase": phase,
            "state_domain_coverage": u64s(state_coverage),
            "visibility_mode": visibility,
        }
        if presence & 1:
            observed = reader.u64()
            require(observed != 0, 34, "observed player id")
            fields["observed_player_entity_id"] = u64s(observed)
        require(authority in (0, 1, 2) and visibility in (0, 1) and phase in (0, 1, 2, 3), 35, "enum")
        require(reserved == 0 and generation >= 1 and producer_id != 0, 34, "SESSION_STATE invariant")
        return fields

    if record_type == 2:
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects no mission name")
        generation = reader.u32()
        phase = reader.u8()
        paused = reader.u8()
        reserved = reader.u16()
        compression = reader.f32()
        sample = reader.u64()
        require(phase <= 4 and paused in (0, 1) and reserved == 0, 34, "MISSION_STATE invariant")
        return {
            "mission_generation": generation,
            "paused": paused,
            "phase": phase,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "time_compression": compression,
        }

    if record_type == 3:
        generation = reader.u32()
        class_id = reader.u32()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base CLASS_MANIFEST")
        name = reader.utf8(255)
        species = reader.u32()
        ship_type = reader.u32()
        mass = reader.f32()
        center = vec3(reader)
        require(generation and class_id and name and mass > 0, 34, "CLASS_MANIFEST invariant")
        return {
            "center_of_mass": center,
            "class_id": class_id,
            "internal_name": name,
            "manifest_generation": generation,
            "mass": mass,
            "presence": u64s(presence),
            "ship_type_id": ship_type,
            "species_id": species,
        }

    if record_type == 4:
        generation = reader.u32()
        weapon_class = reader.u32()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base WEAPON_MANIFEST")
        name = reader.utf8(255)
        subtype = reader.u8()
        flags = reader.u64()
        speed = reader.f32()
        mass = reader.f32()
        gravity = reader.f32()
        lifetime = reader.u64()
        inheritance = reader.f32()
        require(generation and weapon_class and name and subtype <= 5, 34, "WEAPON_MANIFEST invariant")
        return {
            "gravity_multiplier": gravity,
            "internal_name": name,
            "lifetime_us": u64s(lifetime),
            "manifest_generation": generation,
            "mass": mass,
            "max_speed": speed,
            "presence": u64s(presence),
            "subtype": subtype,
            "velocity_inheritance": inheritance,
            "weapon_class_id": weapon_class,
            "weapon_flags": u64s(flags),
        }

    if record_type == 5:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x1ff == 0, 37, "ENTITY_LIFECYCLE presence")
        sample = reader.u64()
        object_type = reader.u8()
        phase = reader.u8()
        flags = reader.u32()
        require(entity and object_type <= 8 and phase <= 5, 34, "ENTITY_LIFECYCLE invariant")
        result = {
            "entity_id": u64s(entity),
            "lifecycle_flags": flags,
            "lifecycle_phase": phase,
            "object_type": object_type,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }
        if presence & 1:
            result["signature"] = reader.u32()
        if presence & 2:
            result["net_signature"] = reader.u32()
        if presence & 4:
            class_id = reader.u32()
            require(class_id != 0, 34, "ENTITY_LIFECYCLE class reference")
            result["class_id"] = class_id
        return result

    if record_type == 6:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base SHIP_IDENTITY")
        sample = reader.u64()
        class_id = reader.u32()
        name = reader.utf8(255)
        species = reader.u32()
        team = reader.u32()
        iff = reader.u32()
        role = reader.u16()
        radius = reader.f32()
        require(entity and class_id and name and team <= 65535 and iff <= 65535, 34, "SHIP_IDENTITY invariant")
        return {
            "entity_id": u64s(entity),
            "iff_id": iff,
            "internal_name": name,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "radius": radius,
            "role_flags": role,
            "ship_class_id": class_id,
            "species_id": species,
            "team_id": team,
        }

    if record_type == 7:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0xfff == 0, 37, "FLIGHT_STATE presence")
        sample = reader.u64()
        position = vec3(reader)
        orientation = [reader.f32(), reader.f32(), reader.f32(), reader.f32()]
        velocity = vec3(reader)
        rotational = vec3(reader)
        radius = reader.f32()
        flags = reader.u32()
        norm = sum(component * component for component in orientation)
        require(entity and abs(norm - 1.0) <= 0.001 and orientation[0] >= 0, 34, "FLIGHT_STATE invariant")
        result = {
            "entity_id": u64s(entity),
            "orientation_local_to_world": orientation,
            "physics_mode_flags": flags,
            "position_world": position,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "radius": radius,
            "rotational_velocity_local": rotational,
            "velocity_world": velocity,
        }
        if presence & 1:
            result["desired_velocity_world"] = vec3(reader)
        return result

    if record_type == 8:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base CONTROL_STATE")
        sample = reader.u64()
        axes = [reader.f32() for _ in range(6)]
        mode = reader.u8()
        flags = reader.u32()
        require(entity and mode <= 4 and all(-1.0 <= value <= 1.0 for value in axes), 34, "CONTROL_STATE invariant")
        return {
            "bank": axes[2],
            "control_flags": flags,
            "control_mode": mode,
            "entity_id": u64s(entity),
            "forward": axes[5],
            "heading": axes[1],
            "pitch": axes[0],
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "sideways": axes[4],
            "vertical": axes[3],
        }

    if record_type == 9:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base DAMAGE_STATE")
        sample = reader.u64()
        hull = reader.f32()
        maximum = reader.f32()
        protection = reader.u16()
        require(entity and maximum > 0 and 0 <= hull <= maximum, 34, "DAMAGE_STATE invariant")
        return {
            "dynamic_max_hull": maximum,
            "entity_id": u64s(entity),
            "hull_strength": hull,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "protection_flags": protection,
        }

    if record_type == 10:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base SHIELD_STATE")
        sample = reader.u64()
        has_shields = reader.u8()
        count = reader.u16()
        reserved = reader.u16()
        require(entity and has_shields in (0, 1) and reserved == 0, 34, "SHIELD_STATE invariant")
        current = [reader.f32() for _ in range(count)]
        maximum = [reader.f32() for _ in range(count)]
        require(count <= 64 and has_shields == (1 if count else 0), 37, "SHIELD_STATE cardinality")
        return {
            "entity_id": u64s(entity),
            "has_shields": has_shields,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "segment_count": count,
            "segment_current_hits": current,
            "segment_max_hits": maximum,
        }

    if record_type == 11:
        entity = reader.u64()
        subsystem = reader.u32()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base SUBSYSTEM_STATE")
        sample = reader.u64()
        index = reader.u16()
        subsystem_type = reader.u8()
        current = reader.f32()
        maximum = reader.f32()
        flags = reader.u32()
        require(entity and subsystem and index <= 1023 and subsystem_type <= 13, 34, "SUBSYSTEM_STATE invariant")
        return {
            "canonical_index": index,
            "current_hits": current,
            "entity_id": u64s(entity),
            "max_hits": maximum,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "subsystem_flags": flags,
            "subsystem_id": subsystem,
            "type": subsystem_type,
        }

    if record_type == 12:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base ENERGY_STATE")
        sample = reader.u64()
        mode, shields, weapons, engines, reserved = [reader.u8() for _ in range(5)]
        require(entity and mode <= 2 and reserved == 0 and max(shields, weapons, engines) <= 12, 34, "ENERGY_STATE invariant")
        return {
            "entity_id": u64s(entity),
            "ets_engines_index": engines,
            "ets_mode": mode,
            "ets_shields_index": shields,
            "ets_weapons_index": weapons,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
        }

    if record_type == 13:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base PROPULSION_STATE")
        sample = reader.u64()
        flags = reader.u16()
        reserved = reader.u16()
        require(entity and reserved == 0, 34, "PROPULSION_STATE invariant")
        return {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "propulsion_flags": flags,
            "reserved": reserved,
        }

    if record_type == 14:
        entity = reader.u64()
        presence = reader.u64()
        require(presence == 0, 37, "fixture profile expects base WEAPON_STATE")
        sample = reader.u64()
        primary, secondary, tertiary, reserved = [reader.u16() for _ in range(4)]
        current_primary = reader.u32()
        current_secondary = reader.u32()
        current_tertiary = reader.u32()
        flags = reader.u32()
        primary_items = reader.u16()
        secondary_items = reader.u16()
        require(entity and primary == primary_items == 0 and secondary == secondary_items == 0 and tertiary == 0 and reserved == 0, 44, "WEAPON_STATE count")
        return {
            "current_primary_bank_id": current_primary,
            "current_secondary_bank_id": current_secondary,
            "current_tertiary_bank_id": current_tertiary,
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "primary_bank_count": primary,
            "primary_banks": [],
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "secondary_bank_count": secondary,
            "secondary_banks": [],
            "tertiary_bank_count": tertiary,
            "weapon_flags": flags,
        }

    if record_type == 15:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        count = reader.u16()
        require(entity and presence == 0 and count == 0, 37, "LOCK_STATE fixture invariant")
        return {
            "entity_id": u64s(entity),
            "locks": [],
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }

    if record_type == 16:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        current = reader.u64()
        require(entity and presence == 0, 37, "TARGET_STATE fixture invariant")
        return {
            "current_target_entity_id": u64s(current),
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }

    if record_type == 17:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        mode = reader.u8()
        selected = reader.f32()
        sensor = reader.u8()
        current = reader.f32()
        maximum = reader.f32()
        require(entity and presence == 0 and mode <= 3 and sensor <= 2 and maximum > 0 and current <= maximum, 34, "RADAR_STATE invariant")
        return {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "radar_mode": mode,
            "selected_range": selected,
            "sensor_current_hits": current,
            "sensor_max_hits": maximum,
            "sensor_state": sensor,
        }

    if record_type == 18:
        entity = reader.u64()
        contact = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        object_type = reader.u8()
        category = reader.u8()
        visibility = reader.u8()
        position = vec3(reader)
        velocity = vec3(reader)
        radius = reader.f32()
        flags = reader.u32()
        require(entity and contact and presence == 0 and object_type <= 8 and category <= 7 and visibility <= 2, 34, "RADAR_CONTACTS invariant")
        return {
            "category": category,
            "contact_entity_id": u64s(contact),
            "contact_flags": flags,
            "entity_id": u64s(entity),
            "object_type": object_type,
            "position_world": position,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "radius": radius,
            "velocity_world": velocity,
            "visibility": visibility,
        }

    if record_type in (19, 20, 21, 22, 23, 24):
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        require(entity and presence == 0, 37, f"{RECORD_NAMES[record_type]} fixture invariant")
        common = {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }
        if record_type == 19:
            level = reader.u8()
            count = reader.u16()
            require(level <= 3 and count == 0, 34, "THREAT_STATE invariant")
            return {**common, "incoming_missiles": [], "threat_level": level}
        if record_type == 20:
            phase = reader.u8()
            disclosure = reader.u8()
            require(phase <= 3 and disclosure <= 1, 35, "CARGO_SCAN_STATE enum")
            return {**common, "disclosure": disclosure, "scan_phase": phase}
        if record_type == 21:
            phase = reader.u8()
            leader = reader.u64()
            count = reader.u16()
            require(phase <= 4 and count == 0, 34, "DOCKING_STATE invariant")
            return {**common, "group_leader_entity_id": u64s(leader), "phase": phase, "relations": []}
        if record_type == 22:
            phase = reader.u8()
            flags = reader.u8()
            reserved = reader.take(3)
            require(phase <= 7 and reserved == bytes(3), 34, "SUPPORT_STATE invariant")
            return {**common, "phase": phase, "reserved": reserved.hex(), "support_flags": flags}
        if record_type == 23:
            state = reader.u8()
            count = reader.u16()
            require(state <= 3 and count == 0, 34, "NAVIGATION_STATE invariant")
            return {**common, "autopilot_state": state, "navpoints": []}
        flags = reader.u32()
        return {**common, "effect_flags": flags}

    if record_type == 25:
        bundle_version = reader.u16()
        manifest_flags = reader.u16()
        bundle_hash = reader.take(32)
        converter_id_length = reader.u8()
        converter_version_length = reader.u8()
        frame_asset = reader.u64()
        placeholder_asset = reader.u64()
        total = reader.u32()
        first = reader.u32()
        count = reader.u16()
        require(bundle_version == 1 and total >= 1 and count >= 1, 34, "COMM_ASSET_MANIFEST prefix")
        require(total <= 4096 and count <= 4096, 46, "COMM_ASSET_MANIFEST quota")
        try:
            converter_id = reader.take(converter_id_length).decode("utf-8", errors="strict")
            converter_version = reader.take(converter_version_length).decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            fail(31, f"invalid converter UTF-8: {exc}")
        entries: list[dict[str, Any]] = []
        seen_ids: set[int] = set()
        for _ in range(count):
            start = reader.offset
            entry_length = reader.u16()
            require(entry_length >= 72 and entry_length - 2 <= reader.remaining, 28, "asset entry length")
            asset_id = reader.u64()
            content_hash = reader.take(32)
            source_format = reader.u8()
            delivered_format = reader.u8()
            alpha_mode = reader.u8()
            timing_mode = reader.u8()
            reserved = reader.u16()
            width = reader.u16()
            height = reader.u16()
            frame_count = reader.u32()
            duration = reader.u64()
            logical_length = reader.u16()
            path_length = reader.u16()
            duration_count = reader.u32()
            require(reserved == 0 and asset_id == int.from_bytes(content_hash[:8], "big"), 44, "asset identity")
            require(asset_id not in seen_ids, 30, "duplicate asset id")
            seen_ids.add(asset_id)
            try:
                logical_name = reader.take(logical_length).decode("utf-8", errors="strict")
                file_path = reader.take(path_length).decode("ascii", errors="strict")
            except UnicodeDecodeError as exc:
                fail(31, f"invalid asset text: {exc}")
            durations = [reader.u32() for _ in range(duration_count)]
            require(reader.offset - start == entry_length, 28, "asset entry did not consume entry_length")
            entries.append(
                {
                    "alpha_mode": alpha_mode,
                    "asset_id": u64s(asset_id),
                    "content_hash": content_hash.hex(),
                    "delivered_format": delivered_format,
                    "duration_us": u64s(duration),
                    "entry_length": entry_length,
                    "file_path": file_path,
                    "frame_count": frame_count,
                    "frame_duration_count": duration_count,
                    "frame_durations_us": durations,
                    "height": height,
                    "logical_name": logical_name,
                    "source_format": source_format,
                    "timing_mode": timing_mode,
                    "width": width,
                }
            )
        return {
            "bundle_hash": bundle_hash.hex(),
            "bundle_version": bundle_version,
            "converter_id": converter_id,
            "converter_version": converter_version,
            "entries": entries,
            "entry_count": count,
            "first_asset_index": first,
            "frame_asset_id": u64s(frame_asset),
            "manifest_flags": manifest_flags,
            "placeholder_asset_id": u64s(placeholder_asset),
            "total_asset_count": total,
        }

    if record_type == 26:
        active = reader.u8()
        playback_mode = reader.u8()
        color_mode = reader.u8()
        stop_reason = reader.u8()
        playback = reader.u64()
        engine_message = reader.u32()
        sender = reader.u64()
        asset = reader.u64()
        sample = reader.u64()
        animation = reader.u64()
        duration = reader.u64()
        rate = reader.f32()
        require(active in (0, 1), 34, "COMM_VIEW_STATE active")
        if active == 0:
            require(not any((playback, engine_message, sender, asset, sample, animation, duration)) and rate == 0.0, 37, "inactive communication fields")
        return {
            "active": active,
            "animation_time_us": u64s(animation),
            "color_mode": color_mode,
            "duration_us": u64s(duration),
            "engine_message_id": engine_message,
            "head_asset_id": u64s(asset),
            "playback_id": u64s(playback),
            "playback_mode": playback_mode,
            "playback_rate": rate,
            "producer_sample_time_us": u64s(sample),
            "sender_entity_id": u64s(sender),
            "stop_reason": stop_reason,
        }

    if record_type == 27:
        event = reader.u64()
        kind = reader.u8()
        playback_mode = reader.u8()
        color_mode = reader.u8()
        stop_reason = reader.u8()
        playback = reader.u64()
        engine_message = reader.u32()
        sender = reader.u64()
        asset = reader.u64()
        sample = reader.u64()
        animation = reader.u64()
        duration = reader.u64()
        rate = reader.f32()
        require(event and kind in (1, 2) and playback, 34, "COMM_VIEW_EVENT invariant")
        return {
            "animation_time_us": u64s(animation),
            "color_mode": color_mode,
            "duration_us": u64s(duration),
            "engine_message_id": engine_message,
            "event_id": u64s(event),
            "event_kind": kind,
            "head_asset_id": u64s(asset),
            "playback_id": u64s(playback),
            "playback_mode": playback_mode,
            "playback_rate": rate,
            "producer_sample_time_us": u64s(sample),
            "sender_entity_id": u64s(sender),
            "stop_reason": stop_reason,
        }

    if record_type == 28:
        count = reader.u16()
        require(1 <= count <= 256, 34, "EVENTS count")
        events = []
        previous = 0
        for _ in range(count):
            version = reader.u8()
            size = reader.u16()
            item = Reader(reader.take(size))
            presence = item.u32()
            event = item.u64()
            sample = item.u64()
            kind = item.u16()
            flags = item.u16()
            subject = item.u64()
            item.finish()
            require(version == 1 and event > previous and 1 <= kind <= 33 and flags & ~7 == 0, 44, "EventItem invariant")
            previous = event
            events.append(
                {
                    "event_flags": flags,
                    "event_id": u64s(event),
                    "item_presence": presence,
                    "item_size": size,
                    "item_version": version,
                    "kind": kind,
                    "producer_sample_time_us": u64s(sample),
                    "subject_entity_id": u64s(subject),
                }
            )
        return {"events": events}

    fail(26, f"unknown record type {record_type}")


def validate_record_semantics(record: dict[str, Any], context: dict[str, Any]) -> None:
    fields = record["fields"]
    if record["recordType"] == 25 and "expectedBundleHash" in context:
        require(fields["bundle_hash"] == context["expectedBundleHash"], 44, "bundle hash mismatch")
    if record["recordType"] in (26, 27) and "assetDurationsUs" in context:
        asset = fields["head_asset_id"]
        durations = context["assetDurationsUs"]
        require(asset in durations, 41, "communication asset missing from installed manifest")
        require(fields["duration_us"] == durations[asset], 44, "communication duration mismatch")


def decode_record(encoded: bytes, context: dict[str, Any] | None = None) -> dict[str, Any]:
    reader = Reader(encoded)
    record_type = reader.u16()
    version = reader.u8()
    flags = reader.u8()
    length = reader.u16()
    require(record_type != 0, 34, "RecordType zero")
    require(record_type in RECORD_NAMES, 26, "unknown required record")
    require(version == 1, 27, "unsupported record version")
    require(length == reader.remaining, 28, "record_length mismatch")
    if record_type in (27, 28):
        require(flags == 1, 36, "event record requires CREATE")
    else:
        require(flags == 0, 36, "canonical record flags")
    payload = Reader(reader.take(length))
    fields = decode_record_payload(record_type, payload)
    payload.finish()
    reader.finish()
    record = canonical_record(record_type, version, flags, length, fields)
    validate_record_semantics(record, context or {})
    return record


def decode_record_region(reader: Reader, count: int) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for _ in range(count):
        require(reader.remaining >= 6, 24, "truncated record envelope")
        start = reader.offset
        length = int.from_bytes(reader.data[start + 4 : start + 6], "little")
        encoded = reader.take(6 + length)
        records.append(decode_record(encoded))
    return records


def canonical_message(message_type: int, flags: int, fields: dict[str, Any]) -> dict[str, Any]:
    return {
        "fields": fields,
        "kind": "message-payload",
        "messageFlags": flags,
        "messageName": MESSAGE_NAMES[message_type],
        "messageType": message_type,
        "schema": SCHEMA,
    }


def decode_extensions(reader: Reader, expected_length: int, expected_count: int) -> list[dict[str, Any]]:
    require(expected_length <= reader.remaining, 24, "truncated extensions")
    extension_reader = Reader(reader.take(expected_length))
    extensions: list[dict[str, Any]] = []
    seen: set[int] = set()
    for _ in range(expected_count):
        extension_type = extension_reader.u16()
        version = extension_reader.u8()
        flags = extension_reader.u8()
        length = extension_reader.u16()
        payload = extension_reader.take(length)
        require(extension_type and extension_type not in seen and flags == 0, 36, "capability extension envelope")
        seen.add(extension_type)
        extensions.append(
            {
                "extension_flags": flags,
                "extension_payload": payload.hex(),
                "extension_type": extension_type,
                "extension_version": version,
            }
        )
    extension_reader.finish()
    return extensions


def enforce_record_count_quota(count: int, context: dict[str, Any]) -> None:
    limit = context.get("recordCountLimit", 65535)
    require(isinstance(limit, int) and 1 <= limit <= 65535, 44, "invalid fixture record-count context")
    require(count <= limit, 46, "record_count exceeds contextual quota")


def validate_singleton_records(records: list[dict[str, Any]]) -> None:
    singleton_types = {1, 2}
    seen: set[int] = set()
    for record in records:
        record_type = record["recordType"]
        if record_type in singleton_types:
            require(record_type not in seen, 29, "duplicate singleton state record")
            seen.add(record_type)


def context_map_value(context: dict[str, Any], name: str, key: int) -> Any:
    mapping = context.get(name)
    if not isinstance(mapping, dict):
        return None
    return mapping.get(str(key), mapping.get(key))


def validate_message_semantics(
    message_type: int, fields: dict[str, Any], context: dict[str, Any]
) -> None:
    if message_type == 6:
        validate_singleton_records(fields["records"])
        if "installedManifestIds" in context:
            require(fields["required_manifest_id"] in context["installedManifestIds"], 41, "snapshot manifest not installed")

    if message_type == 7 and (
        "knownCommittedBaselines" in context or "knownCandidateBaselines" in context
    ):
        baseline = fields["baseline_snapshot_id"]
        committed = context.get("knownCommittedBaselines", [])
        candidates = context.get("knownCandidateBaselines", [])
        # Candidate baselines are known and may be held pending.  A totally
        # unknown baseline is discarded and causes a rate-limited resync.
        require(baseline in committed or baseline in candidates, 39, "totally unknown delta baseline")

    if message_type in (10, 11) and "reliableTarget" in context:
        target = context["reliableTarget"]
        require(context.get("endpoint") == target.get("endpoint"), 14, "ACK/NACK endpoint mismatch")
        require(target.get("pending") is True, 44, "ACK/NACK target is no longer retained")
        require(fields["target_message_id"] == target.get("messageId"), 44, "ACK/NACK message id mismatch")
        require(fields["target_message_type"] == target.get("messageType"), 44, "ACK/NACK message type mismatch")
        require(fields["target_fragment_count"] == target.get("fragmentCount"), 44, "ACK/NACK fragment count mismatch")
        require(fields["target_message_crc32"] == target.get("crc32"), 44, "ACK/NACK message CRC mismatch")

    if message_type == 11:
        fragment_count = fields["target_fragment_count"]
        bitmap = bytes.fromhex(fields["missing_bitmap"])
        require(1 <= fragment_count <= 2048, 34, "NACK fragment count")
        require(len(bitmap) == (fragment_count + 7) // 8, 34, "NACK bitmap length")
        used_tail_bits = fragment_count % 8
        if used_tail_bits:
            require(bitmap[-1] & ~((1 << used_tail_bits) - 1) == 0, 36, "NACK bitmap tail bits")

    if message_type == 15 and "videoNegotiation" in context:
        negotiation = context["videoNegotiation"]
        require(fields["codec"] in negotiation["codecs"], 42, "video codec not negotiated")
        require(fields["codec_profile"] in negotiation["codecProfiles"], 42, "video profile not negotiated")
        require(
            fields["width"] <= negotiation["maxWidth"] and fields["height"] <= negotiation["maxHeight"],
            42,
            "video resolution not negotiated",
        )
        require(fields["bitrate_kbps"] <= negotiation["maxBitrateKbps"], 42, "video bitrate not negotiated")

    if message_type == 16:
        stream = fields["stream_id"]
        if "activeVideoStreams" in context:
            require(stream in context["activeVideoStreams"], 44, "video stream is not active")
        generation = context_map_value(context, "currentVideoGeneration", stream)
        if generation is not None:
            require(fields["config_generation"] == generation, 40, "stale video generation")
        if "knownEntityIds" in context:
            require(int(fields["target_entity_id"]) in context["knownEntityIds"], 38, "unknown video target entity")
        current_target = context_map_value(context, "currentVideoTarget", stream)
        if current_target is not None:
            require(fields["target_entity_id"] == str(current_target), 44, "late frame for previous target")


def decode_message(message_type: int, flags: int, payload: bytes, context: dict[str, Any]) -> dict[str, Any]:
    require(message_type in MESSAGE_NAMES, 11, "unknown message type")
    if "allowedSenderRoles" in context:
        require(context.get("senderRole") in context["allowedSenderRoles"], 12, "message sent in wrong direction")
    reader = Reader(payload)

    if message_type == 1:
        producer = reader.u64()
        port = reader.u16()
        min_major, max_major, min_minor, max_minor = [reader.u8() for _ in range(4)]
        capabilities = reader.u64()
        sequence = reader.u32()
        name_length = reader.u16()
        require(name_length <= 64, 32, "producer name")
        try:
            name = reader.take(name_length).decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            fail(31, f"producer name: {exc}")
        fields = {
            "advert_sequence": sequence,
            "listen_port": port,
            "max_major": max_major,
            "max_minor": max_minor,
            "min_major": min_major,
            "min_minor": min_minor,
            "producer_capabilities": u64s(capabilities),
            "producer_id": u64s(producer),
            "producer_name": name,
        }
    elif message_type == 2:
        nonce = reader.u64()
        t0 = reader.u64()
        min_major, max_major, min_minor, max_minor = [reader.u8() for _ in range(4)]
        visibility = reader.u8()
        reserved = reader.take(3)
        capabilities = reader.u64()
        heartbeat = reader.u16()
        extensions_length = reader.u16()
        extension_count = reader.u16()
        extensions = decode_extensions(reader, extensions_length, extension_count)
        fields = {
            "advertised_capabilities": u64s(capabilities),
            "client_nonce": u64s(nonce),
            "client_send_t0_us": u64s(t0),
            "extension_count": extension_count,
            "extensions": extensions,
            "extensions_length": extensions_length,
            "max_major": max_major,
            "max_minor": max_minor,
            "min_major": min_major,
            "min_minor": min_minor,
            "requested_heartbeat_ms": heartbeat,
            "requested_visibility_mode": visibility,
            "reserved": reserved.hex(),
        }
    elif message_type == 3:
        nonce = reader.u64()
        t0 = reader.u64()
        t1 = reader.u64()
        t2 = reader.u64()
        status = reader.u8()
        major = reader.u8()
        minor = reader.u8()
        visibility = reader.u8()
        producer_capabilities = reader.u64()
        active = reader.u64()
        heartbeat = reader.u16()
        timeout = reader.u16()
        extensions_length = reader.u16()
        extension_count = reader.u16()
        producer = reader.u64()
        extensions = decode_extensions(reader, extensions_length, extension_count)
        fields = {
            "active_capabilities": u64s(active),
            "client_nonce": u64s(nonce),
            "client_send_t0_us": u64s(t0),
            "extension_count": extension_count,
            "extensions": extensions,
            "extensions_length": extensions_length,
            "heartbeat_interval_ms": heartbeat,
            "producer_capabilities": u64s(producer_capabilities),
            "producer_id": u64s(producer),
            "producer_receive_t1_us": u64s(t1),
            "producer_send_t2_us": u64s(t2),
            "reliable_reassembly_timeout_ms": timeout,
            "selected_major": major,
            "selected_minor": minor,
            "selected_visibility_mode": visibility,
            "status": status,
        }
    elif message_type == 4:
        fields = {
            "session_flags": reader.u32(),
            "producer_session_start_us": u64s(reader.u64()),
            "mission_instance_id": u64s(reader.u64()),
            "initial_snapshot_id": reader.u32(),
            "required_manifest_id": reader.u32(),
        }
    elif message_type in (5, 6):
        transaction_id = reader.u32()
        part_index = reader.u16()
        part_count = reader.u16()
        transaction_size = reader.u32()
        transaction_sha = reader.take(32)
        sample = reader.u64()
        if message_type == 5:
            manifest_kind = reader.u16()
            count = reader.u16()
        else:
            required_manifest = reader.u32()
            snapshot_flags = reader.u16()
            count = reader.u16()
        region = reader.data[reader.offset :]
        require(len(region) == transaction_size, 28, "transaction_size mismatch")
        require(hashlib.sha256(region).digest() == transaction_sha, 44, "transaction SHA-256")
        require(count > 0, 34, "empty transaction part")
        enforce_record_count_quota(count, context)
        records = decode_record_region(reader, count)
        fields = {
            ("manifest_id" if message_type == 5 else "snapshot_id"): transaction_id,
            "part_count": part_count,
            "part_index": part_index,
            "producer_sample_time_us": u64s(sample),
            "record_count": count,
            "records": records,
            "transaction_sha256": transaction_sha.hex(),
            "transaction_size": transaction_size,
        }
        if message_type == 5:
            fields["manifest_kind"] = manifest_kind
        else:
            fields["required_manifest_id"] = required_manifest
            fields["snapshot_flags"] = snapshot_flags
    elif message_type == 7:
        baseline = reader.u32()
        sequence = reader.u32()
        sample = reader.u64()
        count = reader.u16()
        reserved = reader.u16()
        require(count > 0, 34, "empty DELTA")
        enforce_record_count_quota(count, context)
        records = decode_record_region(reader, count)
        fields = {
            "baseline_snapshot_id": baseline,
            "delta_sequence": sequence,
            "producer_sample_time_us": u64s(sample),
            "record_count": count,
            "records": records,
            "reserved": reserved,
        }
    elif message_type == 8:
        batch = reader.u32()
        first = reader.u64()
        sample = reader.u64()
        delivery = reader.u8()
        reserved = reader.u8()
        count = reader.u16()
        require(count > 0, 34, "empty EVENT_BATCH")
        enforce_record_count_quota(count, context)
        records = decode_record_region(reader, count)
        fields = {
            "batch_id": batch,
            "delivery_class": delivery,
            "first_event_id": u64s(first),
            "producer_sample_time_us": u64s(sample),
            "record_count": count,
            "records": records,
            "reserved": reserved,
        }
    elif message_type == 9:
        probe = reader.u32()
        kind = reader.u8()
        reserved = reader.take(3)
        origin = reader.u64()
        receive = reader.u64()
        transmit = reader.u64()
        fields = {
            "kind": kind,
            "origin_t0_us": u64s(origin),
            "probe_id": probe,
            "receive_t1_us": u64s(receive),
            "reserved": reserved.hex(),
            "transmit_t2_us": u64s(transmit),
        }
    elif message_type == 10:
        fields = {
            "target_message_id": reader.u32(),
            "target_message_type": reader.u8(),
            "ack_flags": reader.u8(),
            "target_fragment_count": reader.u16(),
            "target_message_crc32": f"0x{reader.u32():08x}",
        }
    elif message_type == 11:
        target_id = reader.u32()
        target_type = reader.u8()
        reason = reader.u8()
        fragment_count = reader.u16()
        crc = reader.u32()
        deadline = reader.u64()
        bitmap_bytes = reader.u16()
        reserved = reader.u16()
        bitmap = reader.take(bitmap_bytes)
        fields = {
            "bitmap_bytes": bitmap_bytes,
            "missing_bitmap": bitmap.hex(),
            "needed_before_producer_time_us": u64s(deadline),
            "reason": reason,
            "reserved": reserved,
            "target_fragment_count": fragment_count,
            "target_message_crc32": f"0x{crc:08x}",
            "target_message_id": target_id,
            "target_message_type": target_type,
        }
    elif message_type == 12:
        fields = {
            "request_id": reader.u32(),
            "reason": reader.u8(),
            "request_flags": reader.u8(),
            "reserved": reader.u16(),
            "last_applied_snapshot_id": reader.u32(),
            "last_applied_delta_sequence": reader.u32(),
            "client_send_time_us": u64s(reader.u64()),
        }
    elif message_type == 13:
        fields = {
            "reason": reader.u8(),
            "end_flags": reader.u8(),
            "reserved": reader.u16(),
            "last_snapshot_id": reader.u32(),
            "producer_sample_time_us": u64s(reader.u64()),
        }
    elif message_type == 14:
        fields = {
            "request_id": reader.u32(),
            "max_width": reader.u16(),
            "max_height": reader.u16(),
            "preferred_width": reader.u16(),
            "preferred_height": reader.u16(),
            "max_fps": reader.u16(),
            "preferred_fps": reader.u16(),
            "max_bitrate_kbps": reader.u32(),
            "preferred_bitrate_kbps": reader.u32(),
            "supported_h264_profiles": reader.u32(),
            "supported_h264_levels": reader.u32(),
            "overlay_capabilities": reader.u32(),
            "acceptable_render_profiles": reader.u32(),
            "preferred_render_profile": reader.u8(),
            "reserved": reader.take(3).hex(),
        }
    elif message_type == 15:
        fields = {
            "request_id": reader.u32(),
            "result": reader.u8(),
            "codec": reader.u8(),
            "codec_profile": reader.u8(),
            "codec_level": reader.u8(),
            "stream_id": reader.u32(),
            "config_generation": reader.u32(),
            "pixel_format": reader.u8(),
            "render_profile": reader.u8(),
            "overlay_mode": reader.u8(),
            "recovery_mode": reader.u8(),
            "width": reader.u16(),
            "height": reader.u16(),
            "fps_num": reader.u16(),
            "fps_den": reader.u16(),
            "bitrate_kbps": reader.u32(),
            "gop_duration_ms": reader.u16(),
            "idr_recovery_window_ms": reader.u16(),
        }
    elif message_type == 16:
        stream = reader.u32()
        generation = reader.u32()
        frame = reader.u32()
        target = reader.u64()
        presentation = reader.u64()
        size = reader.u32()
        video_flags = reader.u16()
        reserved = reader.u16()
        access_unit = reader.take(size)
        require(access_unit.startswith(b"\x00\x00\x00\x01"), 44, "non-canonical Annex B start code")
        fields = {
            "annex_b_access_unit": access_unit.hex(),
            "config_generation": generation,
            "encoded_frame_size": size,
            "presentation_time_us": u64s(presentation),
            "reserved": reserved,
            "stream_id": stream,
            "target_entity_id": u64s(target),
            "video_flags": video_flags,
            "video_frame_id": frame,
        }
    elif message_type == 17:
        fields = {
            "request_id": reader.u32(),
            "stream_id": reader.u32(),
            "config_generation": reader.u32(),
            "last_decodable_frame_id": reader.u32(),
            "needed_before_producer_time_us": u64s(reader.u64()),
            "reason": reader.u8(),
            "reserved": reader.take(7).hex(),
        }
        if context.get("clockFilterValid") is False:
            require(fields["needed_before_producer_time_us"] == "0", 44, "deadline with invalid clock filter")
    elif message_type == 18:
        fields = {
            "request_id": reader.u32(),
            "stream_id": reader.u32(),
            "config_generation": reader.u32(),
            "reason": reader.u8(),
            "stop_flags": reader.u8(),
            "reserved": reader.u16(),
            "producer_time_us": u64s(reader.u64()),
        }
        if context.get("senderRole") == "client":
            require(fields["producer_time_us"] == "0", 44, "client STOP producer time")
    elif message_type == 19:
        names = [
            "stream_id",
            "config_generation",
            "report_id",
            "highest_received_frame_id",
            "highest_decoded_frame_id",
            "highest_presented_frame_id",
            "received_frames",
            "decoded_frames",
            "presented_frames",
            "dropped_frames",
            "missing_fragments",
            "decoder_errors",
            "estimated_loss_ppm",
            "jitter_us",
            "decode_latency_us",
            "presentation_latency_us",
            "buffered_duration_us",
        ]
        fields = {name: reader.u32() for name in names}
        fields["report_interval_ms"] = reader.u16()
        fields["stats_flags"] = reader.u16()
    else:
        generation = reader.u32()
        advertised = reader.u64()
        active = reader.u64()
        effective = reader.u64()
        reason = reader.u8()
        reserved = reader.take(3)
        extensions_length = reader.u16()
        extension_count = reader.u16()
        extensions = decode_extensions(reader, extensions_length, extension_count)
        fields = {
            "active_capabilities": u64s(active),
            "advertised_capabilities": u64s(advertised),
            "capability_generation": generation,
            "effective_time_us": u64s(effective),
            "extension_count": extension_count,
            "extensions": extensions,
            "extensions_length": extensions_length,
            "reason": reason,
            "reserved": reserved.hex(),
        }

    reader.finish()
    validate_message_semantics(message_type, fields, context)
    return canonical_message(message_type, flags, fields)


def crc32_iso_hdlc(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def read_header(datagram: bytes) -> dict[str, int]:
    reader = Reader(datagram[:HEADER_SIZE])
    fields = {
        "magic": reader.u32(),
        "version_major": reader.u8(),
        "version_minor": reader.u8(),
        "message_type": reader.u8(),
        "flags": reader.u8(),
        "header_size": reader.u16(),
        "payload_size": reader.u16(),
        "session_id": reader.u64(),
        "packet_sequence": reader.u32(),
        "frame_id": reader.u32(),
        "mission_time_us": reader.i64(),
        "sent_time_us": reader.u64(),
        "message_id": reader.u32(),
        "fragment_index": reader.u16(),
        "fragment_count": reader.u16(),
        "message_size": reader.u32(),
        "fragment_offset": reader.u32(),
        "message_crc32": reader.u32(),
        "crc32": reader.u32(),
    }
    reader.finish()
    return fields


def decode_transport_sequence(
    datagrams: list[bytes], name: str, context: dict[str, Any] | None = None
) -> dict[str, Any]:
    context = context or {}
    fragments: dict[int, bytes] = {}
    common: tuple[int, ...] | None = None
    header0: dict[str, int] | None = None
    for datagram in datagrams:
        require(len(datagram) >= HEADER_SIZE, 2, "datagram too short")
        require(len(datagram) <= 1200, 3, "datagram too large")
        header = read_header(datagram)
        require(header["magic"] == 0x4C545346, 4, "bad magic")
        require(header["version_major"] == 1, 5, "unsupported major")
        accepted_minor_range = context.get("acceptedMinorRange", [0, 0])
        require(accepted_minor_range[0] <= header["version_minor"] <= accepted_minor_range[1],
                6, "unsupported minor")
        require(header["header_size"] == HEADER_SIZE, 7, "bad header size")
        require(header["flags"] & 0xE0 == 0, 8, "reserved header flag")
        require(header["payload_size"] <= MAX_FRAGMENT_PAYLOAD, 9, "payload too large")
        require(HEADER_SIZE + header["payload_size"] == len(datagram), 9, "bad datagram length")
        sealed = datagram[:64] + bytes(4) + datagram[68:]
        require(crc32_iso_hdlc(sealed) == header["crc32"], 10, "bad datagram CRC")
        message_limit = 2_097_152 if 14 <= header["message_type"] <= 19 else 1_048_576
        require(header["message_size"] <= message_limit, 15, "logical message exceeds class limit")
        if "remainingReassemblyBytes" in context:
            require(
                header["message_size"] <= context["remainingReassemblyBytes"],
                20,
                "reassembly quota exhausted before allocation",
            )
        require(header["fragment_count"] > 0, 16, "zero fragment count")
        require(header["fragment_index"] < header["fragment_count"], 17, "fragment index")
        expected_offset = header["fragment_index"] * MAX_FRAGMENT_PAYLOAD
        require(header["fragment_offset"] == expected_offset, 18, "non-canonical offset")
        require(expected_offset <= header["message_size"], 19, "fragment beyond message")
        expected_slice = min(MAX_FRAGMENT_PAYLOAD, header["message_size"] - expected_offset)
        require(header["payload_size"] == expected_slice, 19, "non-canonical fragment slice")
        identity = (
            header["message_type"],
            header["flags"] & ~0x08,
            header["session_id"],
            header["frame_id"],
            header["mission_time_us"],
            header["message_id"],
            header["fragment_count"],
            header["message_size"],
            header["message_crc32"],
        )
        if common is None:
            common = identity
            header0 = header
        require(identity == common, 21, "inconsistent fragment metadata")
        fragment_index = header["fragment_index"]
        payload = datagram[HEADER_SIZE:]
        if fragment_index in fragments:
            require(fragments[fragment_index] == payload, 21, "contradictory duplicate")
        else:
            fragments[fragment_index] = payload
    require(header0 is not None, 24, "empty sequence")
    require(len(fragments) == header0["fragment_count"], 22, "incomplete sequence")
    logical = b"".join(fragments[index] for index in range(header0["fragment_count"]))
    require(len(logical) == header0["message_size"], 19, "logical size")
    require(crc32_iso_hdlc(logical) == header0["message_crc32"], 23, "bad message CRC")
    return {
        "fragmentCount": header0["fragment_count"],
        "messageCrc32": f"0x{header0['message_crc32']:08x}",
        "messageSize": header0["message_size"],
        "name": name,
        "schema": SCHEMA,
    }


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def verify_protocol_manifest(root: Path) -> None:
    manifest = load_json(root / "protocol-vectors.manifest.json")
    require(manifest.get("schema") == SCHEMA, 44, "manifest schema")
    for entry in manifest.get("files", []):
        path = root / entry["path"]
        require(path.is_file(), 44, f"missing manifested file {entry['path']}")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"], 44, f"hash mismatch {entry['path']}")


def verify_protocol_coverage(root: Path) -> None:
    coverage = load_json(root / "protocol-coverage.json")
    require(coverage.get("schema") == f"{SCHEMA}-catalogue-coverage", 44, "coverage schema")
    valid_catalogue = coverage.get("validCatalogue", [])
    invalid_catalogue = coverage.get("invalidCatalogue", [])
    require(len(valid_catalogue) == 14, 44, "§10.4 coverage item count")
    require(len(invalid_catalogue) == 20, 44, "§10.5 coverage item count")
    item_ids = [entry["id"] for entry in valid_catalogue + invalid_catalogue]
    require(len(item_ids) == len(set(item_ids)), 44, "duplicate catalogue coverage id")
    repo_root = root.parents[2]
    for entry in valid_catalogue + invalid_catalogue:
        evidence = entry.get("evidence", [])
        require(evidence, 44, f"catalogue item has no evidence: {entry['id']}")
        for item in evidence:
            if item["kind"] == "fixture":
                require((root / item["path"]).is_file(), 44, f"missing catalogue fixture {item['path']}")
            elif item["kind"] == "cpp-test":
                source_path = repo_root / item["path"]
                require(source_path.is_file(), 44, f"missing catalogue test source {item['path']}")
                source = source_path.read_text(encoding="utf-8")
                pattern = re.compile(
                    rf"\bTEST\s*\(\s*{re.escape(item['suite'])}\s*,\s*{re.escape(item['name'])}\s*\)"
                )
                require(pattern.search(source) is not None, 44, f"missing exact catalogue test {item['suite']}.{item['name']}")
            else:
                fail(44, f"unknown catalogue evidence kind {item['kind']}")

    category_map = coverage.get("invalidCategoryFixtureMap", {})
    metadata_paths = sorted((root / "vectors/invalid/messages").rglob("*.json"))
    metadata_paths += sorted((root / "vectors/invalid/records").rglob("*.json"))
    categories = {load_json(path)["invalidCategory"] for path in metadata_paths}
    require(set(category_map) == categories, 44, "invalid category coverage map drift")
    for paths in category_map.values():
        require(paths and all((root / path).is_file() for path in paths), 44, "invalid category fixture path drift")


def verify_schema_coverage(root: Path, protocol_metadata: list[Path]) -> None:
    schema = load_json(root / "schema/fstl-v1.yaml")
    schema_messages = {entry["id"] for entry in schema["message_types"]}
    schema_records = {entry["id"] for entry in schema["record_types"]}
    metadata = [load_json(path) for path in protocol_metadata]
    fixture_messages = {entry["messageType"] for entry in metadata if entry["kind"] == "message-payload"}
    fixture_records = {entry["recordType"] for entry in metadata if entry["kind"] == "record"}
    require(schema_messages == fixture_messages == set(range(1, 21)), 44, "MessageType coverage")
    require(schema_records == fixture_records == set(range(1, 29)), 44, "RecordType coverage")


def verify_protocol_fixtures(root: Path) -> tuple[int, int]:
    metadata_paths = sorted((root / "vectors/valid/messages").rglob("*.json"))
    metadata_paths += sorted((root / "vectors/valid/records").rglob("*.json"))
    verify_schema_coverage(root, metadata_paths)
    messages = 0
    records = 0
    for metadata_path in metadata_paths:
        metadata = load_json(metadata_path)
        require(metadata["valid"] is True and metadata["expectedValidationError"] == 0, 44, "valid metadata")
        require(len(metadata["inputFiles"]) == 1, 44, "single payload input")
        encoded = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
        expected = load_json(root / "expected" / metadata["expectedCanonicalJson"])
        try:
            if metadata["kind"] == "message-payload":
                actual = decode_message(
                    metadata["messageType"],
                    metadata.get("messageFlags", 0),
                    encoded,
                    metadata.get("context", {}),
                )
                messages += 1
            elif metadata["kind"] == "record":
                actual = decode_record(encoded)
                records += 1
            else:
                fail(44, f"unexpected protocol fixture kind {metadata['kind']}")
        except DecodeFailure as exc:
            fail(exc.code, f"{metadata_path}: {exc.detail}")
        require(actual == expected, 44, f"canonical JSON mismatch for {metadata_path}")
    return messages, records


REQUIRED_INVALID_PROTOCOL_CATEGORIES = {
    "ack_forged",
    "ack_late",
    "ack_wrong_crc",
    "ack_wrong_endpoint",
    "bool_outside_0_1",
    "client_simulated_command",
    "client_undefined_command",
    "closed_enum_unknown",
    "comm_bundle_inconsistent",
    "comm_bundle_hash_inconsistent",
    "comm_duration_inconsistent",
    "encoded_frame_size_mismatch",
    "fixed_payload_trailing",
    "fixed_payload_truncated",
    "invalid_utf8",
    "nack_bitmap_incoherent",
    "non_finite_float",
    "quota_before_allocation",
    "quaternion_non_canonical",
    "quaternion_non_finite",
    "quaternion_non_normalizable",
    "record_duplicate_item_key",
    "record_duplicate_singleton",
    "record_length_excessive",
    "record_truncated",
    "stale_video_target",
    "string_too_long",
    "unknown_baseline",
    "unknown_generation",
    "unknown_manifest",
    "unknown_stream",
    "unknown_target",
    "video_bitrate_not_negotiated",
    "video_codec_not_negotiated",
    "video_profile_not_negotiated",
    "video_resolution_not_negotiated",
}


def verify_invalid_protocol_fixtures(root: Path) -> tuple[int, int, int]:
    paths = sorted((root / "vectors/invalid/messages").rglob("*.json"))
    paths += sorted((root / "vectors/invalid/records").rglob("*.json"))
    schema = load_json(root / "schema/fstl-v1.yaml")
    validation_names = {entry["id"]: entry["name"] for entry in schema["validation_errors"]}
    messages = 0
    records = 0
    categories: set[str] = set()
    names: set[str] = set()
    for metadata_path in paths:
        metadata = load_json(metadata_path)
        require(metadata["valid"] is False, 44, f"invalid fixture marked valid: {metadata_path}")
        require("expectedCanonicalJson" not in metadata, 44, f"invalid fixture has canonical JSON: {metadata_path}")
        error_expected = metadata["expectedValidationError"]
        require(error_expected in validation_names and error_expected != 0, 44, f"unstable validation code: {metadata_path}")
        require(
            metadata.get("expectedValidationErrorName") == validation_names[error_expected],
            44,
            f"validation code/name drift: {metadata_path}",
        )
        require(len(metadata["inputFiles"]) == 1, 44, f"invalid protocol fixture must have one input: {metadata_path}")
        require(metadata["name"] not in names, 44, f"duplicate invalid fixture name: {metadata['name']}")
        names.add(metadata["name"])
        categories.add(metadata["invalidCategory"])
        encoded = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
        try:
            if metadata["kind"] == "message-payload":
                messages += 1
                decode_message(
                    metadata["messageType"],
                    metadata.get("messageFlags", 0),
                    encoded,
                    metadata.get("context", {}),
                )
            elif metadata["kind"] == "record":
                records += 1
                decode_record(encoded, metadata.get("context", {}))
            else:
                fail(44, f"unexpected invalid protocol fixture kind {metadata['kind']}")
            error_actual = 0
        except DecodeFailure as exc:
            error_actual = exc.code
        require(
            error_actual == error_expected,
            44,
            f"invalid protocol error mismatch for {metadata_path}: expected {error_expected}, got {error_actual}",
        )
    missing = REQUIRED_INVALID_PROTOCOL_CATEGORIES - categories
    require(not missing, 44, f"invalid protocol category coverage missing: {sorted(missing)}")
    return messages, records, len(categories)


def verify_transport_fixtures(root: Path) -> tuple[int, int]:
    valid = 0
    invalid = 0
    paths = sorted((root / "vectors/valid/datagrams/transport").rglob("*.json"))
    paths += sorted((root / "vectors/invalid/datagrams/transport").rglob("*.json"))
    schema = load_json(root / "schema/fstl-v1.yaml")
    validation_names = {entry["id"]: entry["name"] for entry in schema["validation_errors"]}
    for metadata_path in paths:
        metadata = load_json(metadata_path)
        require(
            metadata.get("expectedValidationErrorName") == validation_names[metadata["expectedValidationError"]],
            44,
            f"transport validation code/name drift: {metadata_path}",
        )
        datagrams = [(metadata_path.parent / name).read_bytes() for name in metadata["inputFiles"]]
        try:
            actual = decode_transport_sequence(datagrams, metadata["name"], metadata.get("context", {}))
            error = 0
        except DecodeFailure as exc:
            actual = None
            error = exc.code
        require(error == metadata["expectedValidationError"], 44, f"transport error mismatch for {metadata_path}: got {error}")
        if metadata["valid"]:
            require(actual is not None, 44, f"valid transport fixture rejected: {metadata_path}")
            expected = load_json(root / "expected" / metadata["expectedCanonicalJson"])
            require(actual == expected, 44, f"transport canonical JSON mismatch: {metadata_path}")
            valid += 1
        else:
            require(actual is None and error != 0, 44, f"invalid transport fixture accepted: {metadata_path}")
            invalid += 1
    return valid, invalid


def verify_cross_endian_and_crc() -> None:
    probe = bytes.fromhex("12345678")
    require(int.from_bytes(probe, "little") == 0x78563412, 44, "little-endian simulation")
    require(int.from_bytes(probe, "big") == 0x12345678, 44, "big-endian simulation")
    require(crc32_iso_hdlc(b"123456789") == 0xCBF43926, 44, "CRC-32/ISO-HDLC check value")


def fstl11_snapshot_result(decoded: dict[str, Any], minor: int) -> str:
    records = decoded["fields"]["records"]
    names = [record["recordName"] for record in records]
    if len(set(names)) != len(names):
        return "DuplicateRecord"
    by_name = {record["recordName"]: record["fields"] for record in records}
    session = by_name.get("SESSION_STATE")
    if session is None or "MISSION_STATE" not in by_name:
        return "InvalidAbsence"
    coverage = int(session["state_domain_coverage"])
    if minor == 0 and coverage & 0x400:
        return "ReservedFlag"
    if minor != 1 or not coverage & 0x400:
        return "UNSUPPORTED_VERSION"
    player = session.get("observed_player_entity_id")
    if coverage == 0x400:
        if session["authority_mode"] != 0:
            return "InvalidStateTransition"
        if session["visibility_mode"] != 0:
            return "VisibilityViolation"
        expected = {"SESSION_STATE", "MISSION_STATE"}
        if player is not None:
            expected |= {"ENTITY_LIFECYCLE", "FLIGHT_STATE"}
        if "SHIP_IDENTITY" in by_name:
            return "InvalidAbsence"
        if set(by_name) != expected:
            return "InvalidAbsence"
        if player is not None:
            lifecycle = by_name["ENTITY_LIFECYCLE"]
            flight = by_name["FLIGHT_STATE"]
            if lifecycle["entity_id"] != player or flight["entity_id"] != player:
                return "InvalidAbsence"
            if lifecycle["presence"] != "0" or lifecycle["object_type"] != 1:
                return "InvalidStateTransition"
            if flight["presence"] != "0":
                return "InvalidAbsence"
        return "None"
    core = {"SESSION_STATE", "MISSION_STATE", "ENTITY_LIFECYCLE", "SHIP_IDENTITY", "FLIGHT_STATE",
            "DAMAGE_STATE", "SHIELD_STATE", "SUBSYSTEM_STATE", "ENERGY_STATE", "PROPULSION_STATE"}
    if coverage & 1:
        return "None" if decoded["fields"]["required_manifest_id"] and core <= set(by_name) else "MissingManifest"
    return "INVALID_COVERAGE"


def verify_fstl11_corpus(root: Path) -> int:
    verified = 0
    error_ids = {"None": 0, "DuplicateRecord": 29, "ReservedFlag": 36,
                 "InvalidAbsence": 37, "MissingManifest": 41, "VisibilityViolation": 43,
                 "InvalidStateTransition": 44}
    for metadata_path in sorted((root / "vectors-v1.1").glob("*/*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        kind = metadata["kind"]
        if kind == "datagram":
            encoded = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
            header_minor = int(metadata["headerVersion"].split(".")[1])
            transport = decode_transport_sequence([encoded], metadata["name"],
                                      {"acceptedMinorRange": [header_minor, header_minor]})
            payload = (metadata_path.parent / metadata["payloadFile"]).read_bytes()
            decoded = decode_message(metadata["messageType"], encoded[7], payload, {})
            require(metadata["valid"] and metadata["expectedValidationError"] == 0, 44,
                    f"FSTL 1.1 valid transport metadata drift for {metadata['name']}")
        elif kind == "message-payload" and metadata["messageType"] == 6:
            payload = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
            decoded = decode_message(6, 0, payload, {})
            actual = fstl11_snapshot_result(decoded, int(metadata["versionMinor"]))
            require(error_ids[actual] == metadata["expectedValidationError"] and
                    actual == metadata["expectedValidationErrorName"], 44,
                    f"FSTL 1.1 expected result drift for {metadata['name']}: {actual}")
        else:
            fail(44, f"unexpected FSTL 1.1 corpus kind {kind}")
        canonical = metadata.get("expectedCanonicalJson")
        if canonical:
            expected = json.loads((root / canonical).read_text(encoding="utf-8"))
            decoded["schema"] = "FSTL-1.1"
            if metadata["messageType"] in (6, 7):
                for item in decoded["fields"]["records"]:
                    item["schema"] = "FSTL-1.1"
            require(decoded == expected, 44, f"FSTL 1.1 canonical JSON drift for {metadata['name']}")
        verified += 1
    return verified


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    args = parser.parse_args()
    del args

    root = Path(__file__).resolve().parents[1]
    try:
        verify_cross_endian_and_crc()
        verify_protocol_manifest(root)
        verify_protocol_coverage(root)
        messages, records = verify_protocol_fixtures(root)
        invalid_messages, invalid_records, invalid_categories = verify_invalid_protocol_fixtures(root)
        valid_transport, invalid_transport = verify_transport_fixtures(root)
        fstl11_corpus = verify_fstl11_corpus(root)
    except (DecodeFailure, KeyError, OSError, ValueError, TypeError) as exc:
        if isinstance(exc, DecodeFailure):
            print(f"validation error {exc.code}: {exc.detail}")
        else:
            print(f"fixture verification failed: {exc}")
        return 1
    print(
        f"independent decode passed: {messages} messages, {records} records, "
        f"{invalid_messages} invalid messages and {invalid_records} invalid records "
        f"covering {invalid_categories} negative categories, "
        f"{valid_transport} valid and {invalid_transport} invalid transport fixtures; "
        f"{fstl11_corpus} FSTL 1.1 corpus cases cross-decoded; CRC and simulated cross-endian checks passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

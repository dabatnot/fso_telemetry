#!/usr/bin/env python3
"""Independent standard-library decoder for the current FSTL 1.1 wire format.

The decoder intentionally imports neither the C++ implementation nor the
fixture generator.  It uses a second, bitwise CRC-32/ISO-HDLC calculation and
explicit little-endian reads so the fixture check is meaningful on every host
endianness.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


SCHEMA = "FSTL-1.1"
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
    29: "HUD_ALERT_STATE",
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

    def i16(self) -> int:
        return int.from_bytes(self.take(2), "little", signed=True)

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


def rgba8(reader: Reader) -> list[int]:
    return [reader.u8(), reader.u8(), reader.u8(), reader.u8()]


def mat3(reader: Reader) -> list[list[float]]:
    return [[reader.f32(), reader.f32(), reader.f32()] for _ in range(3)]


def item_reader(reader: Reader) -> tuple[int, int, Reader]:
    version = reader.u8()
    size = reader.u16()
    require(version == 1, 27, "unsupported item version")
    return version, size, Reader(reader.take(size))


def decode_subsystem_animation(reader: Reader) -> dict[str, Any]:
    version, size, item = item_reader(reader)
    presence = item.u16()
    require(presence & ~0x0007 == 0, 37, "subsystem animation presence")
    require(presence & 0x0003, 37, "subsystem animation axes")
    animation_id = item.u16()
    state = item.u8()
    reserved = item.u8()
    require(state <= 4 and reserved == 0, 34, "subsystem animation invariant")
    result: dict[str, Any] = {
        "animation_id": animation_id,
        "item_size": size,
        "item_version": version,
        "presence": presence,
        "state": state,
    }
    if presence & 0x0001:
        result["angle_rad"] = item.f32()
        result["angular_velocity_rad_s"] = item.f32()
    if presence & 0x0002:
        result["translation"] = item.f32()
        result["translation_velocity"] = item.f32()
    if presence & 0x0004:
        if presence & 0x0001:
            result["target_angle_rad"] = item.f32()
        if presence & 0x0002:
            result["target_translation"] = item.f32()
    result["remaining_us"] = u64s(item.u64())
    item.finish()
    return result


def decode_turret_bank(reader: Reader) -> dict[str, Any]:
    version, size, item = item_reader(reader)
    presence = item.u16()
    family = item.u8()
    reserved = item.u8()
    bank_index = item.u16()
    bank_id = item.u32()
    weapon_class_id = item.u32()
    require(
        presence & ~0x0001 == 0
        and family <= 1
        and reserved == 0
        and bank_index <= 63
        and bank_id != 0
        and weapon_class_id != 0,
        34,
        "turret bank invariant",
    )
    result: dict[str, Any] = {
        "bank_id": bank_id,
        "bank_index": bank_index,
        "family": family,
        "item_size": size,
        "item_version": version,
        "presence": presence,
        "weapon_class_id": weapon_class_id,
    }
    if presence & 0x0001:
        result["current_ammo"] = item.u32()
        result["capacity_raw"] = item.f32()
    result["next_fire_remaining_us"] = u64s(item.u64())
    item.finish()
    return result


def decode_turret(reader: Reader) -> dict[str, Any]:
    presence = reader.u32()
    require(presence & ~0x1FFF == 0, 37, "turret presence")
    target_entity = reader.u64()
    result: dict[str, Any] = {
        "target_entity_id": u64s(target_entity),
        "turret_presence": presence,
    }
    if presence & 0x0001:
        target_subsystem = reader.u32()
        require(target_entity != 0 and target_subsystem != 0, 34, "turret target subsystem")
        result["target_subsystem_id"] = target_subsystem
    direction = vec3(reader)
    norm = sum(component * component for component in direction)
    require(abs(norm - 1.0) <= 0.002, 34, "turret direction")
    result["current_direction_local"] = direction
    if presence & 0x0002:
        result["aim_point_world"] = vec3(reader)
        result["estimated_target_velocity_world"] = vec3(reader)
    if presence & 0x0004:
        point = reader.u16()
        reserved = reader.u16()
        require(point <= 255 and reserved == 0, 34, "turret fire point")
        result["next_fire_point_index"] = point
    if presence & 0x0008:
        result["cooldown_remaining_us"] = u64s(reader.u64())
    if presence & 0x0010:
        result["target_in_range_us"] = u64s(reader.u64())
    if presence & 0x0020:
        result["optimal_range"] = reader.f32()
    if presence & 0x0040:
        result["target_priority"] = reader.i16()
        require(reader.u16() == 0, 34, "turret priority reserved")
    if presence & 0x0080:
        result["inaccuracy_rad"] = reader.f32()
    if presence & 0x0100:
        result["fire_rate_multiplier"] = reader.f32()
    if presence & 0x0200:
        state = reader.u8()
        require(state <= 4, 34, "turret animation state")
        result["animation_state"] = state
        result["animation_remaining_us"] = u64s(reader.u64())
    if presence & 0x0400:
        count = reader.u16()
        require(count <= 64, 34, "turret bank count")
        banks = [decode_turret_bank(reader) for _ in range(count)]
        require(
            len({bank["bank_id"] for bank in banks}) == len(banks),
            39,
            "duplicate turret bank",
        )
        result["banks"] = banks
    if presence & 0x0800:
        remaining = reader.u16()
        bank_id = reader.u32()
        require(remaining <= 4096 and bank_id != 0, 34, "turret swarm")
        result["swarm_remaining"] = remaining
        result["swarm_bank_id"] = bank_id
    if presence & 0x1000:
        result["awacs_intensity"] = reader.f32()
        result["awacs_radius"] = reader.f32()
    return result


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


def decode_record_payload(
    record_type: int, record_version: int, reader: Reader
) -> dict[str, Any]:
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
        require(authority in (0, 1, 2) and visibility == 0 and phase in (0, 1, 2, 3), 35, "enum")
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
        require(presence & ~0x3FFF == 0, 37, "CLASS_MANIFEST presence")
        name = reader.utf8(255)
        species = reader.u32()
        ship_type = reader.u32()
        mass = reader.f32()
        center = vec3(reader)
        require(generation and class_id and name and mass > 0, 34, "CLASS_MANIFEST invariant")
        result = {
            "center_of_mass": center,
            "class_id": class_id,
            "internal_name": name,
            "manifest_generation": generation,
            "mass": mass,
            "presence": u64s(presence),
            "ship_type_id": ship_type,
            "species_id": species,
        }
        if presence & 0x0001:
            result["inertia_matrix"] = mat3(reader)
        if presence & 0x0002:
            result["rotational_damping_time"] = vec3(reader)
            result["translational_damping_time"] = vec3(reader)
        if presence & 0x0004:
            result["max_velocity"] = vec3(reader)
            result["afterburner_max_velocity"] = vec3(reader)
            result["booster_max_velocity"] = vec3(reader)
            result["max_rotational_velocity"] = vec3(reader)
            result["max_rear_velocity"] = reader.f32()
            result["forward_accel_time"] = reader.f32()
            result["afterburner_forward_accel_time"] = reader.f32()
            result["booster_forward_accel_time"] = reader.f32()
            result["forward_decel_time"] = reader.f32()
            result["slide_accel_time"] = reader.f32()
            result["slide_decel_time"] = reader.f32()
        if presence & 0x0008:
            result["max_hull_strength"] = reader.f32()
        if presence & 0x0010:
            result["max_shield_strength"] = reader.f32()
        if presence & 0x0020:
            result["max_weapon_energy"] = reader.f32()
            result["weapon_energy_recharge_rate"] = reader.f32()
            result["max_speed_with_engines_disabled"] = reader.f32()
        if presence & 0x0040:
            result["afterburner"] = {
                "fuel_capacity": reader.f32(),
                "burn_rate": reader.f32(),
                "recover_rate": reader.f32(),
                "minimum_start_fuel": reader.f32(),
                "cooldown_us": u64s(reader.u64()),
            }
        if presence & 0x0080:
            result["countermeasure"] = {
                "weapon_class_id": reader.u32(),
                "initial_count": reader.u32(),
                "fire_wait_us": u64s(reader.u64()),
            }
        if presence & 0x0100:
            banks = []
            for _ in range(reader.u16()):
                version, size, item = item_reader(reader)
                item_presence = item.u16()
                require(item_presence & ~0x0003 == 0, 37, "class bank presence")
                bank = {
                    "item_version": version,
                    "item_size": size,
                    "presence": item_presence,
                    "family": item.u8(),
                    "reserved": item.u8(),
                    "canonical_index": item.u16(),
                    "bank_id": item.u32(),
                }
                if item_presence & 0x0002:
                    bank["weapon_class_id"] = item.u32()
                if item_presence & 0x0001:
                    bank["capacity"] = item.f32()
                firepoint_count = item.u16()
                firepoint_version = item.u8()
                firepoint_size = item.u16()
                require(firepoint_version == 1 and firepoint_size == 12, 44, "firepoint envelope")
                bank["fire_points"] = [vec3(item) for _ in range(firepoint_count)]
                item.finish()
                banks.append(bank)
            result["banks"] = banks
        if presence & 0x0200:
            subsystems = []
            for _ in range(reader.u16()):
                version, size, item = item_reader(reader)
                item_presence = item.u16()
                require(item_presence & ~0x000F == 0, 37, "class subsystem presence")
                subsystem = {
                    "item_version": version,
                    "item_size": size,
                    "presence": item_presence,
                    "subsystem_id": item.u32(),
                    "canonical_index": item.u16(),
                    "type": item.u8(),
                    "reserved": item.u8(),
                    "internal_name": item.utf8(255),
                }
                if item_presence & 0x0001:
                    subsystem["alternate_name"] = item.utf8(255)
                if item_presence & 0x0002:
                    subsystem["hud_name"] = item.utf8(255)
                subsystem["position_local"] = vec3(item)
                if item_presence & 0x0004:
                    subsystem["orientation_local"] = [item.f32() for _ in range(4)]
                subsystem["radius"] = item.f32()
                subsystem["max_hits"] = item.f32()
                if item_presence & 0x0008:
                    subsystem["armor_id"] = item.u32()
                subsystem["static_flags"] = item.u32()
                item.finish()
                subsystems.append(subsystem)
            result["subsystems"] = subsystems
        if presence & 0x0400:
            result["scan"] = {
                "required_time_us": u64s(reader.u64()),
                "maximum_distance": reader.f32(),
                "maximum_angle_rad": reader.f32(),
            }
        if presence & 0x0800:
            result["glide_cap"] = reader.f32()
        if presence & 0x1000:
            result["autoaim_fov_rad"] = reader.f32()
        if presence & 0x2000:
            result["radar_icon_id"] = reader.u32()
        return result

    if record_type == 4:
        generation = reader.u32()
        weapon_class = reader.u32()
        presence = reader.u64()
        require(presence & ~0x07FF == 0, 37, "WEAPON_MANIFEST presence")
        name = reader.utf8(255)
        title = reader.utf8(255) if presence & 0x0001 else None
        subtype = reader.u8()
        flags = reader.u64()
        speed = reader.f32()
        acceleration = reader.u64() if presence & 0x0002 else None
        mass = reader.f32()
        gravity = reader.f32()
        lifetime = reader.u64()
        require(generation and weapon_class and name and subtype <= 5, 34, "WEAPON_MANIFEST invariant")
        result = {
            "gravity_multiplier": gravity,
            "internal_name": name,
            "lifetime_us": u64s(lifetime),
            "manifest_generation": generation,
            "mass": mass,
            "max_speed": speed,
            "presence": u64s(presence),
            "subtype": subtype,
            "weapon_class_id": weapon_class,
            "weapon_flags": u64s(flags),
        }
        if title is not None:
            result["title"] = title
        if acceleration is not None:
            result["acceleration_time_us"] = u64s(acceleration)
        if presence & 0x0004:
            result["ranges"] = {
                "minimum": reader.f32(),
                "optimal": reader.f32(),
                "maximum": reader.f32(),
            }
        if presence & 0x0008:
            result["fire"] = {
                "wait_us": u64s(reader.u64()),
                "energy_consumed": reader.f32(),
            }
        if presence & 0x0010:
            result["damage"] = {
                "amount": reader.f32(),
                "damage_type_id": reader.u32(),
                "effect_flags": reader.u32(),
            }
        if presence & 0x0020:
            result["guidance"] = {
                "type": reader.u8(),
                "fov_rad": reader.f32(),
            }
        if presence & 0x0040:
            result["lock"] = {
                "time_us": u64s(reader.u64()),
                "fov_rad": reader.f32(),
            }
        result["velocity_inheritance"] = reader.f32()
        if presence & 0x0080:
            result["cargo_rearm"] = {
                "cargo_size": reader.f32(),
                "rearm_time_us": u64s(reader.u64()),
                "reloaded_per_batch": reader.u32(),
            }
        if presence & 0x0100:
            result["burst"] = {
                "count": reader.u16(),
                "interval_us": u64s(reader.u64()),
            }
        if presence & 0x0200:
            result["swarm"] = {
                "count": reader.u16(),
                "shots_per_trigger": reader.u16(),
            }
        if presence & 0x0400:
            result["countermeasure"] = {
                "effectiveness": reader.f32(),
                "life_us": u64s(reader.u64()),
            }
        return result

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
        require(presence & ~0x1f == 0, 37, "SHIP_IDENTITY presence")
        sample = reader.u64()
        class_id = reader.u32()
        name = reader.utf8(255)
        require(name != "", 34, "SHIP_IDENTITY internal name")
        display_name = reader.utf8(255) if presence & 0x01 else None
        callsign = reader.utf8(127) if presence & 0x02 else None
        species = reader.u32()
        team = reader.u32()
        iff = reader.u32()
        role = reader.u16()
        radius = reader.f32()
        require(
            entity
            and class_id
            and team <= 65535
            and iff <= 65535
            and role & ~0x000f == 0
            and 0.0 <= radius <= 1.0e9,
            34,
            "SHIP_IDENTITY invariant",
        )
        result = {
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
        if display_name is not None:
            result["display_name"] = display_name
        if callsign is not None:
            result["callsign"] = callsign
        if presence & 0x04:
            wing_id = reader.u32()
            wing_position = reader.u16()
            wing_name = reader.utf8(127)
            require(
                wing_id != 0 and wing_position <= 4095 and wing_name != "",
                34,
                "SHIP_IDENTITY wing",
            )
            result["wing"] = {
                "wing_id": wing_id,
                "wing_name": wing_name,
                "wing_position": wing_position,
            }
        if presence & 0x08:
            logical_size = reader.f32()
            require(0.0 <= logical_size <= 1.0e9, 34, "SHIP_IDENTITY logical size")
            result["logical_size"] = logical_size
        if presence & 0x10:
            sensor_visibility = reader.u16()
            require(
                sensor_visibility & ~0x000f == 0,
                34,
                "SHIP_IDENTITY sensor visibility",
            )
            result["sensor_visibility_flags"] = sensor_visibility
        return result

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
        require(presence & ~0x0007 == 0, 37, "CONTROL_STATE presence")
        sample = reader.u64()
        axes = [reader.f32() for _ in range(6)]
        mode = reader.u8()
        flags = reader.u32()
        require(
            entity
            and mode <= 4
            and flags & ~0x0000003F == 0
            and all(-1.0 <= value <= 1.0 for value in axes),
            34,
            "CONTROL_STATE invariant",
        )
        result = {
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
        if presence & 0x0001:
            cruise = reader.f32()
            require(-100.0 <= cruise <= 100.0, 34, "CONTROL_STATE cruise")
            result["forward_cruise_percent"] = cruise
        if presence & 0x0002:
            result["fire_primary_count"] = reader.u16()
            result["fire_secondary_count"] = reader.u16()
            result["fire_countermeasure_count"] = reader.u16()
        if presence & 0x0004:
            cursor_pitch = reader.f32()
            cursor_yaw = reader.f32()
            sensitivity = reader.f32()
            deadzone = reader.f32()
            require(
                -math.pi <= cursor_pitch <= math.pi
                and -math.pi <= cursor_yaw <= math.pi
                and 0.0 <= sensitivity <= 1.0
                and 0.0 <= deadzone <= 1.0,
                34,
                "CONTROL_STATE flight cursor",
            )
            result["flight_cursor"] = {
                "cursor_deadzone": deadzone,
                "cursor_pitch_rad": cursor_pitch,
                "cursor_sensitivity": sensitivity,
                "cursor_yaw_rad": cursor_yaw,
            }
        return result

    if record_type == 9:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x003F == 0, 37, "DAMAGE_STATE presence")
        sample = reader.u64()
        hull = reader.f32()
        maximum = reader.f32()
        protection = reader.u16()
        require(
            entity
            and maximum > 0
            and maximum <= 1.0e12
            and 0 <= hull <= maximum
            and protection & ~0x0007 == 0,
            34,
            "DAMAGE_STATE invariant",
        )
        result = {
            "dynamic_max_hull": maximum,
            "entity_id": u64s(entity),
            "hull_strength": hull,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "protection_flags": protection,
        }
        if presence & 0x0001:
            sim_hull = reader.f32()
            require(0.0 <= sim_hull <= maximum, 34, "DAMAGE_STATE sim hull")
            result["sim_hull_strength"] = sim_hull
        if presence & 0x0002:
            armor_id = reader.u32()
            require(armor_id != 0, 34, "DAMAGE_STATE armor")
            result["armor_id"] = armor_id
        if presence & 0x0004:
            threshold = reader.f32()
            require(0.0 <= threshold <= maximum, 34, "DAMAGE_STATE guardian")
            result["guardian_threshold"] = threshold
        if presence & 0x0008:
            cumulative = reader.f32()
            require(0.0 <= cumulative <= 1.0e12, 34, "DAMAGE_STATE cumulative")
            result["cumulative_damage"] = cumulative
        if presence & 0x0010:
            result["last_damage_source_entity_id"] = u64s(reader.u64())
            result["last_damage_weapon_class_id"] = reader.u32()
        if presence & 0x0020:
            count = reader.u16()
            item_version = reader.u8()
            item_size = reader.u16()
            require(
                count <= 64 and item_version == 1 and item_size == 20,
                34,
                "DAMAGE_STATE contributors envelope",
            )
            contributors = []
            keys = set()
            for _ in range(count):
                source = reader.u64()
                weapon = reader.u32()
                reserved = reader.u32()
                accumulated = reader.f32()
                require(
                    reserved == 0 and 0.0 <= accumulated <= 1.0e12,
                    34,
                    "DAMAGE_STATE contributor",
                )
                key = (source, weapon)
                require(key not in keys, 39, "duplicate damage contributor")
                keys.add(key)
                contributors.append(
                    {
                        "accumulated_damage": accumulated,
                        "source_entity_id": u64s(source),
                        "weapon_class_id": weapon,
                    }
                )
            result["contributors"] = contributors
        return result

    if record_type == 10:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x0007 == 0, 37, "SHIELD_STATE presence")
        sample = reader.u64()
        has_shields = reader.u8()
        count = reader.u16()
        reserved = reader.u16()
        require(entity and has_shields in (0, 1) and reserved == 0, 34, "SHIELD_STATE invariant")
        current = [reader.f32() for _ in range(count)]
        maximum = [reader.f32() for _ in range(count)]
        require(
            count <= 64
            and has_shields == (1 if count else 0)
            and all(0.0 < value <= 1.0e12 for value in maximum)
            and all(0.0 <= value <= maximum[index] for index, value in enumerate(current))
            and (has_shields or presence == 0),
            37,
            "SHIELD_STATE cardinality",
        )
        result = {
            "entity_id": u64s(entity),
            "has_shields": has_shields,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "segment_count": count,
            "segment_current_hits": current,
            "segment_max_hits": maximum,
        }
        if presence & 0x0001:
            recharge_max = reader.f32()
            require(0.0 <= recharge_max <= 1.0e12, 34, "SHIELD_STATE recharge")
            result["recharge_max"] = recharge_max
        if presence & 0x0002:
            regeneration = reader.f32()
            require(0.0 <= regeneration <= 1.0e12, 34, "SHIELD_STATE regeneration")
            result["regeneration_per_s"] = regeneration
        if presence & 0x0004:
            deferred = reader.f32()
            require(-1.0e12 <= deferred <= 1.0e12, 34, "SHIELD_STATE deferred transfer")
            result["deferred_energy_transfer"] = deferred
        return result

    if record_type == 11:
        entity = reader.u64()
        subsystem = reader.u32()
        presence = reader.u64()
        require(presence & ~0x00FF == 0, 37, "SUBSYSTEM_STATE presence")
        sample = reader.u64()
        index = reader.u16()
        subsystem_type = reader.u8()
        current = reader.f32()
        maximum = reader.f32()
        flags = reader.u32()
        require(
            entity
            and subsystem
            and index <= 1023
            and subsystem_type <= 13
            and flags & ~0x000000FF == 0
            and 0.0 <= current <= maximum <= 1.0e12,
            34,
            "SUBSYSTEM_STATE invariant",
        )
        result = {
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
        if presence & 0x0001:
            internal = reader.utf8(255)
            require(internal != "", 34, "SUBSYSTEM_STATE internal override")
            result["name_overrides"] = {
                "alternate_name_override": reader.utf8(255),
                "hud_name_override": reader.utf8(255),
                "internal_name_override": internal,
            }
        if presence & 0x0002:
            armor_id = reader.u32()
            require(armor_id != 0, 34, "SUBSYSTEM_STATE armor")
            result["armor_id"] = armor_id
        has_perturbation = bool(presence & 0x0004)
        require(
            has_perturbation == bool(flags & 0x0001),
            37,
            "SUBSYSTEM_STATE perturbation",
        )
        if has_perturbation:
            result["perturbation_remaining_us"] = u64s(reader.u64())
        if presence & 0x0008:
            translation = vec3(reader)
            orientation = [reader.f32() for _ in range(4)]
            norm = sum(component * component for component in orientation)
            require(abs(norm - 1.0) <= 0.001 and orientation[0] >= 0.0, 34, "SUBSYSTEM_STATE transform")
            result["animated_translation_local"] = translation
            result["animated_orientation_local"] = orientation
        if presence & 0x0010:
            animation_count = reader.u16()
            require(animation_count <= 64, 34, "SUBSYSTEM_STATE animation count")
            animations = [decode_subsystem_animation(reader) for _ in range(animation_count)]
            require(
                len({item["animation_id"] for item in animations}) == len(animations),
                39,
                "duplicate subsystem animation",
            )
            result["animations"] = animations
        if presence & 0x0020:
            disclosure = reader.u8()
            cargo_text = reader.utf8(511)
            require(
                disclosure <= 1 and (disclosure != 0 or cargo_text == ""),
                34,
                "SUBSYSTEM_STATE cargo",
            )
            result["cargo_disclosure"] = disclosure
            result["cargo_text"] = cargo_text
        if presence & 0x0040:
            aggregate_current = reader.f32()
            aggregate_maximum = reader.f32()
            require(
                0.0 <= aggregate_current <= aggregate_maximum <= 1.0e12,
                34,
                "SUBSYSTEM_STATE aggregate",
            )
            result["aggregate_current_hits"] = aggregate_current
            result["aggregate_max_hits"] = aggregate_maximum
        if presence & 0x0080:
            require(subsystem_type == 2, 37, "SUBSYSTEM_STATE turret type")
            result["turret"] = decode_turret(reader)
        return result

    if record_type == 12:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x003F == 0, 37, "ENERGY_STATE presence")
        sample = reader.u64()
        mode, shields, weapons, engines, reserved = [reader.u8() for _ in range(5)]
        require(
            entity
            and mode <= 2
            and reserved == 0
            and max(shields, weapons, engines) <= 12
            and (mode != 0 or (shields == 0 and weapons == 0 and engines == 0)),
            34,
            "ENERGY_STATE invariant",
        )
        result = {
            "entity_id": u64s(entity),
            "ets_engines_index": engines,
            "ets_mode": mode,
            "ets_shields_index": shields,
            "ets_weapons_index": weapons,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
        }
        if presence & 0x0001:
            current_energy = reader.f32()
            maximum_energy = reader.f32()
            require(
                0.0 <= current_energy <= maximum_energy <= 1.0e12 and maximum_energy > 0.0,
                34,
                "ENERGY_STATE weapon energy",
            )
            result["weapon_energy_current"] = current_energy
            result["weapon_energy_max"] = maximum_energy
        if presence & 0x0002:
            result["weapon_regeneration_per_s"] = reader.f32()
            result["shield_regeneration_per_s"] = reader.f32()
        if presence & 0x0004:
            result["deferred_to_weapons"] = reader.f32()
            result["deferred_to_shields"] = reader.f32()
        if presence & 0x0008:
            result["resulting_engine_power"] = reader.f32()
            result["resulting_max_speed"] = reader.f32()
        if presence & 0x0010:
            result["power_output"] = reader.f32()
        if presence & 0x0020:
            aggregate_current = reader.f32()
            aggregate_maximum = reader.f32()
            require(
                0.0 <= aggregate_current <= aggregate_maximum <= 1.0e12
                and aggregate_maximum > 0.0,
                34,
                "ENERGY_STATE engine integrity",
            )
            result["aggregate_engine_current_hits"] = aggregate_current
            result["aggregate_engine_max_hits"] = aggregate_maximum
        return result

    if record_type == 13:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x003F == 0, 37, "PROPULSION_STATE presence")
        sample = reader.u64()
        flags = reader.u16()
        reserved = reader.u16()
        has_fuel = bool(presence & 0x0001)
        require(
            entity
            and reserved == 0
            and flags & ~0x00FF == 0
            and (has_fuel or not (presence & 0x0006))
            and (has_fuel or not (flags & 0x000F))
            and (not (flags & 0x0004) or ((flags & 0x0001) and not (flags & 0x0002))),
            34,
            "PROPULSION_STATE invariant",
        )
        result = {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "propulsion_flags": flags,
            "reserved": reserved,
        }
        fuel_maximum = 0.0
        if has_fuel:
            fuel_current = reader.f32()
            fuel_maximum = reader.f32()
            require(
                0.0 <= fuel_current <= fuel_maximum <= 1.0e12 and fuel_maximum > 0.0,
                34,
                "PROPULSION_STATE fuel",
            )
            result["fuel_current"] = fuel_current
            result["fuel_max"] = fuel_maximum
        if presence & 0x0002:
            result["consumption_per_s"] = reader.f32()
            result["recovery_per_s"] = reader.f32()
        if presence & 0x0004:
            minimum = reader.f32()
            cooldown = reader.u64()
            since_stop = reader.u64()
            engagement_fuel = reader.f32()
            require(
                0.0 <= minimum <= fuel_maximum
                and cooldown <= 3_600_000_000
                and since_stop <= 86_400_000_000
                and 0.0 <= engagement_fuel <= fuel_maximum,
                34,
                "PROPULSION_STATE engagement",
            )
            result["minimum_to_engage"] = minimum
            result["cooldown_remaining_us"] = u64s(cooldown)
            result["time_since_last_stop_us"] = u64s(since_stop)
            result["fuel_at_last_engagement"] = engagement_fuel
        if presence & 0x0008:
            acceleration = reader.f32()
            velocities = vec3(reader)
            require(
                0.0 <= acceleration <= 3600.0
                and all(0.0 <= value <= 1.0e9 for value in velocities),
                34,
                "PROPULSION_STATE dynamics",
            )
            result["forward_accel_time_const_s"] = acceleration
            result["afterburner_max_velocity_local"] = velocities
        if presence & 0x0010:
            wash = reader.f32()
            require(0.0 <= wash <= 1.0e12, 34, "PROPULSION_STATE wash")
            result["engine_wash_intensity"] = wash
        if presence & 0x0020:
            rcs = vec3(reader)
            require(all(-1.0 <= value <= 1.0 for value in rcs), 34, "PROPULSION_STATE RCS")
            result["rcs_intensity"] = rcs
        return result

    if record_type == 14:
        entity = reader.u64()
        presence = reader.u64()
        require(presence & ~0x00FF == 0, 37, "WEAPON_STATE presence")
        sample = reader.u64()
        primary, secondary, tertiary, reserved = [reader.u16() for _ in range(4)]
        current_primary = reader.u32()
        current_secondary = reader.u32()
        current_tertiary = reader.u32()
        flags = reader.u32()
        require(entity and reserved == 0, 34, "WEAPON_STATE invariant")
        result = {
            "current_primary_bank_id": current_primary,
            "current_secondary_bank_id": current_secondary,
            "current_tertiary_bank_id": current_tertiary,
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "primary_bank_count": primary,
            "producer_sample_time_us": u64s(sample),
            "reserved": reserved,
            "secondary_bank_count": secondary,
            "tertiary_bank_count": tertiary,
            "weapon_flags": flags,
        }
        if presence & 0x0001:
            result["previous_primary_bank_id"] = reader.u32()
        if presence & 0x0002:
            result["previous_secondary_bank_id"] = reader.u32()
        if presence & 0x0004:
            result["targeting_laser_bank_id"] = reader.u32()
        if presence & 0x0008:
            result["swarm"] = {
                "remaining": reader.u16(),
                "origin_bank_id": reader.u32(),
            }
        if presence & 0x0010:
            result["remote_detonation_remaining_us"] = u64s(reader.u64())
        if presence & 0x0020:
            result["per_burst_rotation"] = reader.f32()
        primary_items = reader.u16()
        require(primary == primary_items, 44, "WEAPON_STATE primary count")
        primary_banks = []
        for _ in range(primary_items):
            version, size, item = item_reader(reader)
            item_presence = item.u16()
            require(item_presence & ~0x003F == 0, 37, "primary bank presence")
            bank = {
                "item_version": version,
                "item_size": size,
                "presence": item_presence,
                "canonical_index": item.u16(),
                "bank_id": item.u32(),
                "weapon_class_id": item.u32(),
                "cooldown_remaining_us": u64s(item.u64()),
                "primary_slot": item.u16(),
                "fire_point": item.u16(),
                "simultaneous_slots": item.u16(),
                "pattern_id": item.u16(),
            }
            if item_presence & 0x0001:
                bank["ammunition"] = {
                    "current": item.u32(),
                    "initial": item.u32(),
                    "capacity": item.f32(),
                }
            if item_presence & 0x0002:
                bank.setdefault("ammunition", {})["rearm_remaining_us"] = u64s(item.u64())
            if item_presence & 0x0004:
                bank["burst"] = {
                    "counter": item.u16(),
                    "reserved": item.u16(),
                    "seed": item.u32(),
                }
            if item_presence & 0x0008:
                bank["substitution_pattern_index"] = item.u16()
                bank["substitution_reserved"] = item.u16()
            if item_presence & 0x0010:
                bank["animation"] = {
                    "position": item.f32(),
                    "velocity": item.f32(),
                }
            if item_presence & 0x0020:
                bank["fof_cooldown_remaining_us"] = u64s(item.u64())
            item.finish()
            primary_banks.append(bank)
        secondary_items = reader.u16()
        require(secondary == secondary_items, 44, "WEAPON_STATE secondary count")
        secondary_banks = []
        for _ in range(secondary_items):
            version, size, item = item_reader(reader)
            item_presence = item.u16()
            require(item_presence & ~0x001F == 0, 37, "secondary bank presence")
            bank = {
                "item_version": version,
                "item_size": size,
                "presence": item_presence,
                "canonical_index": item.u16(),
                "bank_id": item.u32(),
                "weapon_class_id": item.u32(),
                "cooldown_remaining_us": u64s(item.u64()),
                "secondary_slot": item.u16(),
                "reserved": item.u16(),
            }
            if item_presence & 0x0001:
                bank["ammunition"] = {
                    "current": item.u32(),
                    "initial": item.u32(),
                    "capacity": item.f32(),
                }
            if item_presence & 0x0002:
                bank.setdefault("ammunition", {})["rearm_remaining_us"] = u64s(item.u64())
            if item_presence & 0x0004:
                bank["burst"] = {
                    "counter": item.u16(),
                    "reserved": item.u16(),
                    "seed": item.u32(),
                }
            if item_presence & 0x0008:
                bank["substitution_pattern_index"] = item.u16()
                bank["substitution_reserved"] = item.u16()
            if item_presence & 0x0010:
                bank["animation"] = {
                    "position": item.f32(),
                    "velocity": item.f32(),
                }
            item.finish()
            secondary_banks.append(bank)
        result["primary_banks"] = primary_banks
        result["secondary_banks"] = secondary_banks
        require((presence & 0x0040 != 0) == (tertiary > 0), 44, "WEAPON_STATE tertiary presence")
        if presence & 0x0040:
            result["tertiary"] = {
                "bank_id": reader.u32(),
                "ammunition_current": reader.u32(),
                "ammunition_initial": reader.u32(),
                "ammunition_capacity": reader.f32(),
                "cooldown_remaining_us": u64s(reader.u64()),
                "rearm_remaining_us": u64s(reader.u64()),
            }
        if presence & 0x0080:
            countermeasure_presence = reader.u16()
            countermeasure = {
                "presence": countermeasure_presence,
                "flags": reader.u16(),
            }
            if countermeasure_presence & 0x0001:
                countermeasure["weapon_class_id"] = reader.u32()
            countermeasure["current"] = reader.u32()
            countermeasure["maximum"] = reader.u32()
            countermeasure["cooldown_remaining_us"] = u64s(reader.u64())
            result["countermeasure"] = countermeasure
        return result

    if record_type == 15:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        count = reader.u16()
        require(entity and presence == 0 and count <= 64, 37, "LOCK_STATE invariant")
        locks = []
        identities: set[tuple[int, int | tuple[float, float, float]]] = set()
        for _ in range(count):
            version, size, item = item_reader(reader)
            item_presence = item.u16()
            require(item_presence & ~0x0003 == 0, 37, "LOCK_STATE item presence")
            locked = item.u8()
            in_cone = item.u8()
            target = item.u64()
            require(locked <= 1 and in_cone <= 1 and target, 34, "LOCK_STATE item invariant")
            lock: dict[str, Any] = {
                "item_version": version,
                "item_size": size,
                "presence": item_presence,
                "locked": bool(locked),
                "target_in_lock_cone": bool(in_cone),
                "target_entity_id": u64s(target),
            }
            subsystem = 0
            if item_presence & 0x0001:
                subsystem = item.u32()
                require(subsystem, 34, "LOCK_STATE subsystem")
                lock["subsystem_id"] = subsystem
            position = vec3(item)
            lock["world_position"] = position
            if item_presence & 0x0002:
                lock["time_to_lock_remaining_us"] = u64s(item.u64())
            item.finish()
            identity: tuple[int, int | tuple[float, float, float]] = (
                target,
                subsystem if subsystem else tuple(position),
            )
            require(identity not in identities, 30, "duplicate LOCK_STATE item")
            identities.add(identity)
            locks.append(lock)
        return {
            "entity_id": u64s(entity),
            "locks": locks,
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }

    if record_type == 16:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        current = reader.u64()
        require(entity and presence & ~0xFFFFF == 0, 37, "TARGET_STATE presence")
        require(current or presence & ~0x0001 == 0, 37, "TARGET_STATE absent target")
        result = {
            "current_target_entity_id": u64s(current),
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }
        if presence & 0x0001:
            result["previous_target_entity_id"] = u64s(reader.u64())
        if presence & 0x0002:
            object_type = reader.u8()
            require(object_type <= 8, 34, "TARGET_STATE object type")
            result["revealed_identity"] = {
                "object_type": object_type,
                "name": reader.utf8(255),
                "class_id": reader.u32(),
                "team_id": reader.u32(),
                "iff_id": reader.u32(),
            }
        if presence & 0x0004:
            result["time_on_target_us"] = u64s(reader.u64())
        if presence & 0x0008:
            result["target_subsystem_id"] = reader.u32()
        if presence & 0x0010:
            result["lock_subsystem_id"] = reader.u32()
        if presence & 0x0020:
            result["last_stealth_position"] = vec3(reader)
            result["last_stealth_velocity"] = vec3(reader)
        if presence & 0x0040:
            result["distance_trend"] = reader.u8()
        if presence & 0x0080:
            result["speed_trend"] = reader.u8()
        if presence & 0x0100:
            in_cone = reader.u8()
            require(in_cone <= 1, 34, "TARGET_STATE in cone")
            result["in_cone"] = bool(in_cone)
        if presence & 0x0200:
            result["lead_world"] = vec3(reader)
            result["lead_bank_id"] = reader.u32()
        if presence & 0x0400:
            result["attacker_entity_id"] = u64s(reader.u64())
        if presence & 0x0800:
            result["dangerous_weapon_entity_id"] = u64s(reader.u64())
        if presence & 0x1000:
            result["nearest_locked_entity_id"] = u64s(reader.u64())
        if presence & 0x2000:
            result["exact_hud_distance"] = reader.f32()
        if presence & 0x4000:
            require(record_version >= 2, 37, "TARGET_STATE v2 speed")
            result["exact_hud_speed"] = reader.f32()
        if presence & 0x8000:
            require(record_version >= 3, 37, "TARGET_STATE v3 HUD label")
            result["hud_type_label"] = reader.utf8(255)
        if presence & 0x10000:
            require(record_version >= 4, 37, "TARGET_STATE v4 HUD color")
            result["hud_target_color"] = rgba8(reader)
        if presence & 0x20000:
            require(record_version >= 5, 37, "TARGET_STATE v5 target subsystem label")
            result["hud_target_subsystem_label"] = reader.utf8(255)
        if presence & 0x40000:
            require(record_version >= 5, 37, "TARGET_STATE v5 lock subsystem label")
            result["hud_lock_subsystem_label"] = reader.utf8(255)
        if presence & 0x80000:
            require(record_version >= 6, 37, "TARGET_STATE v6 HUD target strength")
            identity = result.get("revealed_identity")
            require(isinstance(identity, dict) and identity.get("object_type") == 1,
                    37, "TARGET_STATE HUD target strength ship disclosure")
            hull_ratio = reader.f32()
            has_shields = reader.u8()
            require(0.0 <= hull_ratio <= 1.0 and has_shields <= 1,
                    34, "TARGET_STATE HUD target strength")
            strength = {
                "hull_ratio": hull_ratio,
                "has_shields": bool(has_shields),
            }
            if has_shields:
                shield_ratio = reader.f32()
                require(0.0 <= shield_ratio <= 1.0,
                        34, "TARGET_STATE HUD shield ratio")
                strength["shield_ratio"] = shield_ratio
            result["hud_target_strength"] = strength
        return result

    if record_type == 17:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        mode = reader.u8()
        selected = reader.f32()
        sensor = reader.u8()
        current = reader.f32()
        maximum = reader.f32()
        require(entity and presence & ~0x003F == 0 and mode <= 3 and sensor <= 2 and maximum > 0 and current <= maximum, 34, "RADAR_STATE invariant")
        result = {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "radar_mode": mode,
            "selected_range": selected,
            "sensor_current_hits": current,
            "sensor_max_hits": maximum,
            "sensor_state": sensor,
        }
        if presence & 0x0001:
            result["bright_range"] = reader.f32()
        if presence & 0x0002:
            result["primitive_range"] = reader.f32()
        if presence & 0x0004:
            result["awacs_intensity"] = reader.f32()
            result["awacs_range"] = reader.f32()
        if presence & 0x0008:
            result["emp_intensity"] = reader.f32()
            result["emp_remaining_us"] = u64s(reader.u64())
        if presence & 0x0010:
            result["jamming_intensity"] = reader.f32()
            result["distortion_intensity"] = reader.f32()
        if presence & 0x0020:
            result["first_visible_time_us"] = u64s(reader.u64())
            result["last_contact_time_us"] = u64s(reader.u64())
        return result

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
        radar_local_position = vec3(reader) if record_version >= 2 else None
        radar_projection_distance = (
            reader.f32() if record_version >= 2 else None
        )
        radius = reader.f32()
        flags = reader.u32()
        require(entity and contact and presence & ~0x00FF == 0 and object_type <= 8 and category <= 7 and visibility <= 2, 34, "RADAR_CONTACTS invariant")
        require(flags & ~0xFF == 0, 37, "RADAR_CONTACTS flags")
        require(not (flags & 0x20) or object_type == 2, 35,
                "RADAR_CONTACTS bomb object type")
        result = {
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
        if record_version >= 2:
            result["radar_local_position"] = radar_local_position
            result["radar_projection_distance"] = radar_projection_distance
        if presence & 0x0001:
            result["icon_size"] = reader.f32()
        if presence & 0x0002:
            result["revealed_name"] = reader.utf8(255)
        if presence & 0x0004:
            result["revealed_class_id"] = reader.u32()
        if presence & 0x0008:
            result["revealed_team_id"] = reader.u32()
            result["revealed_iff_id"] = reader.u32()
        if presence & 0x0010:
            result["first_detection_time_us"] = u64s(reader.u64())
            result["last_detection_time_us"] = u64s(reader.u64())
        if presence & 0x0020:
            result["confidence"] = reader.f32()
        if presence & 0x0040:
            require(record_version >= 3, 37, "RADAR_CONTACTS v3 HUD label")
            require(object_type == 1 and visibility == 1, 37,
                    "RADAR_CONTACTS HUD label visibility")
            label = reader.utf8(255)
            require(bool(label), 34, "RADAR_CONTACTS empty HUD label")
            result["hud_type_label"] = label
        if presence & 0x0080:
            require(record_version == 4, 37, "RADAR_CONTACTS v4 radar visual")
            result["radar_blip_color"] = rgba8(reader)
            blip_type = reader.u8()
            require(blip_type <= 5, 34, "RADAR_CONTACTS radar blip type")
            require(bool(flags & 0x20) == (blip_type == 2), 35,
                    "RADAR_CONTACTS bomb visual")
            require(bool(flags & 0x08) == (blip_type == 4), 35,
                    "RADAR_CONTACTS tagged visual")
            require(bool(flags & 0x10) == (blip_type == 3), 35,
                    "RADAR_CONTACTS warp visual")
            require(not (flags & 0x02) or bool(flags & 0x01), 35,
                    "RADAR_CONTACTS selected visual must be bright")
            compatible_object = (
                object_type == 5 if blip_type == 0 else
                object_type == 2 if blip_type == 2 else
                object_type in (1, 2) if blip_type == 3 else
                object_type == 1
            )
            require(compatible_object, 35,
                    "RADAR_CONTACTS blip type object compatibility")
            result["radar_blip_type"] = blip_type
        return result

    if record_type == 29:
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        primary_fire = reader.u8()
        lock_state = reader.u8()
        require(entity != 0, 34, "HUD_ALERT_STATE entity")
        require(presence & ~0x0001 == 0, 36, "HUD_ALERT_STATE presence")
        require(primary_fire <= 1, 34, "HUD_ALERT_STATE primary fire bool")
        require(lock_state <= 2, 35, "HUD_ALERT_STATE lock state")
        result = {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
            "primary_fire_threat_active": bool(primary_fire),
            "missile_lock_state": lock_state,
        }
        if presence & 0x0001:
            warning_kind = reader.u8()
            warning_instance = reader.u64()
            warning_remaining = reader.u64()
            warning_text = reader.utf8(511)
            require(1 <= warning_kind <= 7, 35, "HUD_ALERT_STATE warning kind")
            require(warning_instance != 0 and warning_remaining != 0,
                    34, "HUD_ALERT_STATE warning identity/duration")
            require(bool(warning_text), 34, "HUD_ALERT_STATE warning text")
            result.update({
                "warning_kind": warning_kind,
                "warning_instance_id": u64s(warning_instance),
                "warning_remaining_us": u64s(warning_remaining),
                "warning_text": warning_text,
            })
        return result

    if record_type in (19, 20, 21, 22, 23, 24):
        entity = reader.u64()
        presence = reader.u64()
        sample = reader.u64()
        require(entity, 34, f"{RECORD_NAMES[record_type]} entity")
        common = {
            "entity_id": u64s(entity),
            "presence": u64s(presence),
            "producer_sample_time_us": u64s(sample),
        }
        if record_type == 19:
            require(presence & ~0x0007 == 0, 37, "THREAT_STATE presence")
            level = reader.u8()
            result = {**common, "threat_level": level}
            if presence & 0x0001:
                result["nearest_attacker_entity_id"] = u64s(reader.u64())
            if presence & 0x0002:
                result["dangerous_weapon_entity_id"] = u64s(reader.u64())
            if presence & 0x0004:
                result["nearest_homing_entity_id"] = u64s(reader.u64())
            count = reader.u16()
            require(level <= 3 and count <= 256, 34, "THREAT_STATE invariant")
            missiles = []
            missile_ids: set[int] = set()
            for _ in range(count):
                version, size, item = item_reader(reader)
                item_presence = item.u16()
                guidance = item.u8()
                visibility = item.u8()
                missile_id = item.u64()
                weapon_class_id = item.u32()
                target_id = item.u64()
                require(item_presence & ~0x0001 == 0 and guidance <= 5 and visibility <= 2, 37, "incoming missile invariant")
                require(missile_id and weapon_class_id and target_id == entity, 34, "incoming missile references")
                missile = {
                    "item_version": version,
                    "item_size": size,
                    "presence": item_presence,
                    "guidance_type": guidance,
                    "radar_visibility": visibility,
                    "entity_id": u64s(missile_id),
                    "weapon_class_id": weapon_class_id,
                    "target_entity_id": u64s(target_id),
                }
                if item_presence & 0x0001:
                    missile["homing_subsystem_id"] = item.u32()
                missile["position_world"] = vec3(item)
                missile["orientation_local_to_world"] = [item.f32() for _ in range(4)]
                missile["velocity_world"] = vec3(item)
                item.finish()
                require(missile_id not in missile_ids, 30, "duplicate incoming missile")
                missile_ids.add(missile_id)
                missiles.append(missile)
            result["incoming_missiles"] = missiles
            return result
        if record_type == 20:
            require(presence & ~0x001F == 0, 37, "CARGO_SCAN_STATE presence")
            phase = reader.u8()
            disclosure = reader.u8()
            require(phase <= 3 and disclosure <= 1, 35, "CARGO_SCAN_STATE enum")
            result = {**common, "disclosure": disclosure, "scan_phase": phase}
            if presence & 0x0001:
                result["target_entity_id"] = u64s(reader.u64())
            if presence & 0x0002:
                result["target_subsystem_id"] = reader.u32()
            if presence & 0x0004:
                result["elapsed_us"] = u64s(reader.u64())
                result["required_us"] = u64s(reader.u64())
            if presence & 0x0008:
                result["validity_flags"] = reader.u8()
            if presence & 0x0010:
                result["cargo_text"] = reader.utf8(511)
            return result
        if record_type == 21:
            require(presence == 0, 37, "DOCKING_STATE presence")
            phase = reader.u8()
            leader = reader.u64()
            count = reader.u16()
            require(phase <= 4 and count <= 64, 34, "DOCKING_STATE invariant")
            relations = []
            for _ in range(count):
                version, size, item = item_reader(reader)
                relation = {
                    "item_version": version,
                    "item_size": size,
                    "remote_entity_id": u64s(item.u64()),
                    "local_dockpoint": item.u16(),
                    "remote_dockpoint": item.u16(),
                    "local_dock_bay_name": item.utf8(127),
                    "remote_dock_bay_name": item.utf8(127),
                }
                item.finish()
                relations.append(relation)
            return {**common, "group_leader_entity_id": u64s(leader), "phase": phase, "relations": relations}
        if record_type == 22:
            require(presence & ~0x0001 == 0, 37, "SUPPORT_STATE presence")
            phase = reader.u8()
            flags = reader.u8()
            reserved = reader.take(3)
            require(phase <= 7 and reserved == bytes(3), 34, "SUPPORT_STATE invariant")
            result = {**common, "phase": phase, "reserved": reserved.hex(), "support_flags": flags}
            if presence & 0x0001:
                result["support_entity_id"] = u64s(reader.u64())
            return result
        if record_type == 23:
            require(presence & ~0x0007 == 0, 37, "NAVIGATION_STATE presence")
            state = reader.u8()
            count = reader.u16()
            require(state <= 3 and count <= 1024, 34, "NAVIGATION_STATE invariant")
            navpoints = []
            navpoint_ids: set[int] = set()
            for _ in range(count):
                version, size, item = item_reader(reader)
                item_presence = item.u16()
                nav_type = item.u8()
                nav_flags = item.u8()
                nav_id = item.u32()
                require(item_presence & ~0x0003 == 0 and nav_type <= 2 and nav_id, 37, "navpoint invariant")
                navpoint = {
                    "item_version": version,
                    "item_size": size,
                    "presence": item_presence,
                    "type": nav_type,
                    "flags": nav_flags,
                    "navpoint_id": nav_id,
                    "name": item.utf8(255),
                    "position_world": vec3(item),
                }
                if item_presence & 0x0001:
                    navpoint["linked_entity_id"] = u64s(item.u64())
                if item_presence & 0x0002:
                    navpoint["waypoint_list_id"] = item.u32()
                    navpoint["waypoint_index"] = item.u16()
                item.finish()
                require(nav_id not in navpoint_ids, 30, "duplicate navpoint")
                navpoint_ids.add(nav_id)
                navpoints.append(navpoint)
            result = {**common, "autopilot_state": state, "navpoints": navpoints}
            if presence & 0x0001:
                result["current_navpoint_id"] = reader.u32()
            if presence & 0x0002:
                result["autopilot_refusal"] = reader.u8()
            if presence & 0x0004:
                route_count = reader.u16()
                version = reader.u8()
                size = reader.u16()
                require(
                    0 < route_count <= 2048 and version == 1 and size == 20,
                    34,
                    "route waypoint list header",
                )
                route = []
                for _ in range(route_count):
                    waypoint = {
                        "waypoint_list_id": reader.u32(),
                        "waypoint_index": reader.u16(),
                    }
                    require(reader.u16() == 0, 37, "route waypoint reserved")
                    waypoint["position_world"] = vec3(reader)
                    route.append(waypoint)
                result["route_waypoints"] = route
                result["current_route_index"] = reader.u16()
                result["route_speed_limit"] = reader.f32()
            return result
        require(presence == 0, 37, "EFFECT_STATE presence")
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


def decode_delete_key(record_type: int, reader: Reader) -> dict[str, Any]:
    if record_type == 5:
        entity = reader.u64()
        require(entity != 0, 34, "ENTITY_LIFECYCLE DELETE key")
        return {"entity_id": u64s(entity)}
    if record_type == 11:
        entity = reader.u64()
        subsystem = reader.u32()
        require(entity != 0 and subsystem != 0, 34, "SUBSYSTEM_STATE DELETE key")
        return {"entity_id": u64s(entity), "subsystem_id": subsystem}
    if record_type == 18:
        observer = reader.u64()
        contact = reader.u64()
        require(observer != 0 and contact != 0, 34, "RADAR_CONTACTS DELETE key")
        return {
            "contact_entity_id": u64s(contact),
            "entity_id": u64s(observer),
        }
    fail(36, "DELETE is not supported for this record")


def decode_record(
    encoded: bytes,
    context: dict[str, Any] | None = None,
    container: str = "standalone",
) -> dict[str, Any]:
    reader = Reader(encoded)
    record_type = reader.u16()
    version = reader.u8()
    flags = reader.u8()
    length = reader.u16()
    require(record_type != 0, 34, "RecordType zero")
    require(record_type in RECORD_NAMES, 26, "unknown required record")
    phase3_v2 = record_type in (16, 18) and version == 2
    phase3_v3 = record_type in (16, 18) and version == 3
    phase3_v4 = record_type in (16, 18) and version == 4
    phase3_v5 = record_type == 16 and version == 5
    phase3_v6 = record_type == 16 and version == 6
    require(version == 1 or phase3_v2 or phase3_v3 or phase3_v4 or
            phase3_v5 or phase3_v6, 27, "unsupported record version")
    require(length == reader.remaining, 28, "record_length mismatch")
    if container == "event" or (container == "standalone" and record_type in (27, 28)):
        require(record_type in (27, 28), 36, "state record in event batch")
        require(flags == 1, 36, "event record requires CREATE")
    elif container == "delta":
        if record_type in (5, 11, 18):
            require(flags in (0, 1, 2), 36, "DELTA state record flags")
        else:
            require(flags == 0, 36, "canonical DELTA record flags")
    else:
        require(flags == 0, 36, "canonical record flags")
    payload = Reader(reader.take(length))
    fields = (
        decode_delete_key(record_type, payload)
        if container == "delta" and flags == 2
        else decode_record_payload(record_type, version, payload)
    )
    payload.finish()
    reader.finish()
    record = canonical_record(record_type, version, flags, length, fields)
    if (context or {}).get("retainEncodedRecord") is True:
        record["_encodedRecordHex"] = encoded.hex()
    validate_record_semantics(record, context or {})
    return record


def decode_record_region(
    reader: Reader, count: int, container: str, context: dict[str, Any] | None = None
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for _ in range(count):
        require(reader.remaining >= 6, 24, "truncated record envelope")
        start = reader.offset
        length = int.from_bytes(reader.data[start + 4 : start + 6], "little")
        encoded = reader.take(6 + length)
        records.append(decode_record(encoded, context=context, container=container))
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
        # ``transaction_size`` and ``transaction_sha`` describe the ordered
        # concatenation of *all* transaction parts, not this one part's
        # record-region.  A single-part decoder must therefore validate only
        # its own bounded structure; the console transaction receiver checks
        # cross-part metadata, aggregate size and aggregate SHA-256 before
        # publishing atomically.
        require(transaction_id != 0 and 0 < part_count <= 64 and part_index < part_count,
                34, "transaction part bounds")
        require(0 < transaction_size <= 16_777_216 and 0 < len(region) <= transaction_size,
                28, "transaction part size")
        require(count > 0, 34, "empty transaction part")
        enforce_record_count_quota(count, context)
        records = decode_record_region(
            reader, count, "manifest" if message_type == 5 else "snapshot", context
        )
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
        records = decode_record_region(reader, count, "delta", context)
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
        records = decode_record_region(reader, count, "event", context)
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
        accepted_minor_range = context.get("acceptedMinorRange", [1, 1])
        require(accepted_minor_range == [1, 1] and header["version_minor"] == 1,
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


def verify_cross_endian_and_crc() -> None:
    probe = bytes.fromhex("12345678")
    require(int.from_bytes(probe, "little") == 0x78563412, 44, "little-endian simulation")
    require(int.from_bytes(probe, "big") == 0x12345678, 44, "big-endian simulation")
    require(crc32_iso_hdlc(b"123456789") == 0xCBF43926, 44, "CRC-32/ISO-HDLC check value")


def fstl11_snapshot_result(decoded: dict[str, Any]) -> str:
    records = decoded["fields"]["records"]
    names = [record["recordName"] for record in records]
    if len(set(names)) != len(names):
        return "DuplicateRecord"
    by_name = {record["recordName"]: record["fields"] for record in records}
    session = by_name.get("SESSION_STATE")
    if session is None or "MISSION_STATE" not in by_name:
        return "InvalidAbsence"
    coverage = int(session["state_domain_coverage"])
    if not coverage & 0x400:
        return "INVALID_COVERAGE"
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
    schema_path = root / "schema" / "fstl-v1.1.yaml"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    require(schema.get("wire_version") == "1.1", 44,
            "schema/fstl-v1.1.yaml wire_version drift")
    coverage = schema["numeric_registries"]["StateDomainCoverage"]
    player_values = [item.get("value") for item in coverage["values"]
                     if item.get("name") == "PLAYER_KINEMATICS"]
    require(player_values == [0x400], 44,
            "schema/fstl-v1.1.yaml PLAYER_KINEMATICS drift")
    provenance = schema.get("amendment_provenance", {})
    require(provenance.get("normative") is False, 44,
            "schema/fstl-v1.1.yaml provenance must be informative")

    verified = 0
    error_ids = {"None": 0, "DuplicateRecord": 29,
                 "InvalidAbsence": 37, "MissingManifest": 41, "VisibilityViolation": 43,
                 "InvalidStateTransition": 44}
    for metadata_path in sorted((root / "vectors-v1.1").glob("*/*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        kind = metadata["kind"]
        if kind == "datagram":
            encoded = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
            header_minor = int(metadata["headerVersion"].split(".")[1])
            require(header_minor == 1, 44,
                    f"non-current FSTL transport case {metadata['name']}")
            transport = decode_transport_sequence([encoded], metadata["name"],
                                      {"acceptedMinorRange": [header_minor, header_minor]})
            payload = (metadata_path.parent / metadata["payloadFile"]).read_bytes()
            decoded = decode_message(metadata["messageType"], encoded[7], payload, {})
            require(metadata["valid"] and metadata["expectedValidationError"] == 0, 44,
                    f"FSTL 1.1 valid transport metadata drift for {metadata['name']}")
        elif kind == "message-payload" and metadata["messageType"] == 6:
            payload = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
            try:
                decoded = decode_message(6, 0, payload, {})
            except DecodeFailure as exc:
                require(not metadata["valid"] and
                        exc.code == metadata["expectedValidationError"] and
                        exc.code == 35 and metadata["expectedValidationErrorName"] == "UnknownEnum",
                        44, f"FSTL 1.1 decode result drift for {metadata['name']}: {exc.code}")
                decoded = None
            if decoded is not None:
                require(metadata["versionMinor"] == 1, 44,
                        f"non-current FSTL payload case {metadata['name']}")
                actual = fstl11_snapshot_result(decoded)
                require(error_ids[actual] == metadata["expectedValidationError"] and
                        actual == metadata["expectedValidationErrorName"], 44,
                        f"FSTL 1.1 expected result drift for {metadata['name']}: {actual}")
        elif kind == "record":
            encoded = (metadata_path.parent / metadata["inputFiles"][0]).read_bytes()
            decoded = decode_record(encoded, metadata.get("context", {}), "standalone")
            require(metadata["valid"] and metadata["expectedValidationError"] == 0,
                    44, f"FSTL 1.1 valid record metadata drift for {metadata['name']}")
        else:
            fail(44, f"unexpected FSTL 1.1 corpus kind {kind}")
        canonical = metadata.get("expectedCanonicalJson")
        if canonical and decoded is not None:
            expected = json.loads((root / canonical).read_text(encoding="utf-8"))
            decoded["schema"] = "FSTL-1.1"
            if metadata.get("messageType") in (6, 7):
                for item in decoded["fields"]["records"]:
                    item["schema"] = "FSTL-1.1"
            require(decoded == expected, 44, f"FSTL 1.1 canonical JSON drift for {metadata['name']}")
        verified += 1
    return verified


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()

    root = args.repo.resolve() / "test" / "telemetry" / "protocol"
    try:
        verify_cross_endian_and_crc()
        fstl11_corpus = verify_fstl11_corpus(root)
    except (DecodeFailure, KeyError, OSError, ValueError, TypeError) as exc:
        if isinstance(exc, DecodeFailure):
            print(f"validation error {exc.code}: {exc.detail}")
        else:
            print(f"fixture verification failed: {exc}")
        return 1
    print(
        f"independent decode passed: {fstl11_corpus} FSTL 1.1 corpus cases "
        "cross-decoded; CRC and simulated cross-endian checks passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

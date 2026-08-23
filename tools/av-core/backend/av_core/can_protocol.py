from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum

from .models import CockpitStatus, LampTestRequest, LightingConfig


CAN_BITRATE = 1_000_000
WARNING_STATE_ID = 0x180
CAUTION_STATE_ID = 0x181
LIGHTING_COMMAND_ID = 0x182
LIGHTING_STATE_ID = 0x183
THREAT_STATE_ID = 0x184
NODE_STATUS_IDS = {
    "WARN_CTRL": 0x700,
    "THREAT_PROC": 0x701,
    "SENS_PROC": 0x702,
    "INST_PROC": 0x703,
}

PROTOCOL_VERSION = 1
LAMP_TEST_DURATION_MS = 2000


class LightingOpcode(IntEnum):
    WARNING_COLOR = 1
    CAUTION_COLOR = 2
    LIMITS = 3
    START_TEST = 4


class LampTestTarget(IntEnum):
    ALL = 0
    WARN_CTRL = 1
    LAMP = 2
    THREAT_PROC = 3
    NONE = 0xFF


LAMP_IDS = {
    name: index
    for index, name in enumerate(
        (
            "MASTER_WARNING", "FIRE", "MISSILE", "BLAST", "COLLISION", "EMP",
            "MASTER_CAUTION", "ENG", "SENS", "SHIELD", "HULL", "WEP_EN",
            "AB_FUEL", "AMMO", "CM_LOW", "SUBSYS", "AV_CORE", "FLT_DATA",
            "AV_BUS", "SENS_PROC", "THREAT_PROC", "INST_PROC", "WARN_CTRL",
            "THREAT_FORWARD", "THREAT_FORWARD_RIGHT", "THREAT_RIGHT",
            "THREAT_AFT_RIGHT", "THREAT_AFT", "THREAT_AFT_LEFT", "THREAT_LEFT",
            "THREAT_FORWARD_LEFT", "THREAT_LOCK",
        )
    )
}

THREAT_LAMP_NAMES = frozenset(name for name in LAMP_IDS if name.startswith("THREAT_")) - {"THREAT_PROC"}

COCKPIT_CAUTION_NAMES = (
    "engine", "sensor", "shield", "hull", "weapon_energy", "afterburner_fuel",
    "ammo", "countermeasures", "subsystem",
)
DIAGNOSTIC_NAMES = (
    "AV_CORE", "FLT_DATA", "AV_BUS", "SENS_PROC", "THREAT_PROC", "INST_PROC", "WARN_CTRL",
)


def _frame(*values: int) -> bytes:
    if len(values) > 8 or any(value < 0 or value > 255 for value in values):
        raise ValueError("CAN payload values must fit in eight bytes")
    return bytes(values) + bytes(8 - len(values))


def encode_warning_state(cockpit: CockpitStatus) -> bytes:
    warning = cockpit.warnings
    mask = sum(
        (1 << bit) if active else 0
        for bit, active in enumerate((warning.fire, warning.missile, warning.blast, warning.collision, warning.emp))
    )
    return _frame(PROTOCOL_VERSION, mask)


def decode_warning_state(payload: bytes) -> int:
    _validate_payload(payload)
    if payload[0] != PROTOCOL_VERSION or payload[1] & ~0x1F:
        raise ValueError("invalid WARNING_STATE")
    return payload[1]


def encode_threat_state(cockpit: CockpitStatus, *, live: bool) -> bytes:
    threat = cockpit.threat
    if not live or not threat.available:
        return _frame(PROTOCOL_VERSION, 0, 0)
    lock = {"NONE": 0, "ATTEMPT": 1, "ACQUIRED": 2}[threat.lock_state]
    return _frame(PROTOCOL_VERSION, threat.sector_mask & 0xFF, lock)


def decode_threat_state(payload: bytes) -> tuple[int, int]:
    _validate_payload(payload)
    if payload[0] != PROTOCOL_VERSION or payload[2] not in (0, 1, 2) or any(payload[3:]):
        raise ValueError("invalid THREAT_STATE")
    return payload[1], payload[2]


def encode_caution_state(cockpit: CockpitStatus, diagnostics: dict[str, int]) -> bytes:
    caution = cockpit.cautions
    cockpit_mask = 0
    for bit, name in enumerate(COCKPIT_CAUTION_NAMES):
        if getattr(caution, name).state == "ACTIVE":
            cockpit_mask |= 1 << bit
    diagnostic_mask = 0
    for bit, name in enumerate(DIAGNOSTIC_NAMES):
        state = diagnostics.get(name, 0)
        if state not in (0, 1, 2):
            raise ValueError(f"invalid diagnostic state for {name}")
        diagnostic_mask |= state << (bit * 2)
    return _frame(
        PROTOCOL_VERSION,
        cockpit_mask & 0xFF,
        (cockpit_mask >> 8) & 0xFF,
        diagnostic_mask & 0xFF,
        (diagnostic_mask >> 8) & 0xFF,
    )


def decode_caution_state(payload: bytes) -> tuple[int, int]:
    _validate_payload(payload)
    if payload[0] != PROTOCOL_VERSION:
        raise ValueError("invalid CAUTION_STATE")
    cockpit_mask = payload[1] | payload[2] << 8
    diagnostic_mask = payload[3] | payload[4] << 8
    if cockpit_mask & ~0x01FF or diagnostic_mask & ~0x3FFF:
        raise ValueError("reserved CAUTION_STATE bits are set")
    return cockpit_mask, diagnostic_mask


def encode_lighting_configuration(config: LightingConfig) -> list[bytes]:
    slow_centihz = round(config.slow_flash_hz * 100)
    fast_centihz = round(config.fast_flash_hz * 100)
    return [
        _frame(PROTOCOL_VERSION, LightingOpcode.WARNING_COLOR, config.warning_color.r, config.warning_color.g, config.warning_color.b),
        _frame(PROTOCOL_VERSION, LightingOpcode.CAUTION_COLOR, config.caution_color.r, config.caution_color.g, config.caution_color.b),
        _frame(
            PROTOCOL_VERSION,
            LightingOpcode.LIMITS,
            config.max_brightness_percent,
            slow_centihz & 0xFF,
            slow_centihz >> 8,
            fast_centihz & 0xFF,
            fast_centihz >> 8,
        ),
    ]


def encode_lamp_test(request: LampTestRequest) -> bytes:
    target = LampTestTarget[request.target]
    lamp = LAMP_IDS[request.lamp] if request.lamp is not None else 0xFF
    return _frame(
        PROTOCOL_VERSION,
        LightingOpcode.START_TEST,
        target,
        lamp,
        LAMP_TEST_DURATION_MS & 0xFF,
        LAMP_TEST_DURATION_MS >> 8,
    )


@dataclass(frozen=True)
class LightingState:
    brightness_percent: int
    test_target: LampTestTarget
    lamp_id: int | None
    physical_test_active: bool


def decode_lighting_state(payload: bytes) -> LightingState:
    _validate_payload(payload)
    if payload[0] != PROTOCOL_VERSION or payload[1] > 100:
        raise ValueError("invalid LIGHTING_STATE")
    try:
        target = LampTestTarget(payload[2])
    except ValueError as exc:
        raise ValueError("invalid LIGHTING_STATE target") from exc
    lamp = None if payload[3] == 0xFF else payload[3]
    if lamp is not None and lamp not in LAMP_IDS.values():
        raise ValueError("invalid LIGHTING_STATE lamp")
    if payload[4] not in (0, 1):
        raise ValueError("invalid LIGHTING_STATE physical flag")
    return LightingState(payload[1], target, lamp, bool(payload[4]))


@dataclass(frozen=True)
class NodeStatus:
    role: str
    health: str
    firmware_version: str
    uid: str


def decode_node_status(arbitration_id: int, payload: bytes) -> NodeStatus:
    _validate_payload(payload)
    role = next((role for role, can_id in NODE_STATUS_IDS.items() if can_id == arbitration_id), None)
    if role is None or payload[0] != PROTOCOL_VERSION or payload[1] not in (0, 1):
        raise ValueError("invalid NODE_STATUS")
    uid = int.from_bytes(payload[5:8], "little")
    return NodeStatus(
        role=role,
        health="ONLINE" if payload[1] == 0 else "DEGRADED",
        firmware_version=f"{payload[2]}.{payload[3]}.{payload[4]}",
        uid=f"{uid:06X}",
    )


def _validate_payload(payload: bytes) -> None:
    if len(payload) != 8:
        raise ValueError("CAN payload must contain exactly eight bytes")

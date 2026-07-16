#!/usr/bin/env python3
"""Generate and verify the isolated FSTL 1.1 PLAYER_KINEMATICS corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import re
import zlib
from pathlib import Path

DEFAULT_REPO = Path(__file__).resolve().parents[4]
REPO = DEFAULT_REPO
ROOT = REPO / "test" / "telemetry" / "protocol"
V1_VECTORS = ROOT / "vectors"
V11 = ROOT / "vectors-v1.1"
MANIFEST = ROOT / "fstl-1.1-vectors.manifest.json"
V11_SCHEMA = ROOT / "schema" / "fstl-v1.1.yaml"
V10_LEDGER = ROOT / "fstl-1.0-artifacts.manifest.json"
FROZEN_V10_ARTIFACT_COUNT = 438
FROZEN_V10_ARTIFACT_TREE_SHA256 = "9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d"
PLAYER_KINEMATICS = 0x400
CORE_SHIP = 0x001
SESSION_STATE = 1
MISSION_STATE = 2
ENTITY_LIFECYCLE = 5
SHIP_IDENTITY = 6
FLIGHT_STATE = 7
CORE_RECORDS = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13}
ERROR_IDS = {"None": 0, "DuplicateRecord": 29, "ReservedFlag": 36,
             "InvalidAbsence": 37, "MissingManifest": 41, "VisibilityViolation": 43,
             "InvalidStateTransition": 44}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def configure_repo(repo: Path) -> None:
    global REPO, ROOT, V1_VECTORS, V11, MANIFEST, V11_SCHEMA, V10_LEDGER
    REPO = repo.resolve()
    ROOT = REPO / "test" / "telemetry" / "protocol"
    V1_VECTORS = ROOT / "vectors"
    V11 = ROOT / "vectors-v1.1"
    MANIFEST = ROOT / "fstl-1.1-vectors.manifest.json"
    V11_SCHEMA = ROOT / "schema" / "fstl-v1.1.yaml"
    V10_LEDGER = ROOT / "fstl-1.0-artifacts.manifest.json"


def frozen_v10_contract() -> tuple[int, str]:
    ledger = json.loads(V10_LEDGER.read_text(encoding="utf-8"))
    count = ledger.get("fileCount")
    tree = ledger.get("treeSha256")
    if ledger.get("schema") != "FSTL-1.0-FROZEN-ARTIFACTS" or ledger.get("wireVersion") != "1.0":
        raise ValueError("FSTL 1.0 frozen-artifact ledger identity drift")
    if count != FROZEN_V10_ARTIFACT_COUNT or tree != FROZEN_V10_ARTIFACT_TREE_SHA256:
        raise ValueError(
            f"FSTL 1.0 frozen-artifact ledger drift: count={count!r}, tree={tree!r}"
        )
    return count, tree


def record(record_type: int, payload: bytes) -> bytes:
    return struct.pack("<HBBH", record_type, 1, 0, len(payload)) + payload


def session_state(player: int | None, coverage: int, authority: int = 0, visibility: int = 0) -> bytes:
    presence = 1 if player is not None else 0
    payload = struct.pack(
        "<QQQBBBBIQQQQ", presence, 0x0102030405060708, 1_000_000,
        authority, visibility, 2, 0, 1, 0, coverage, 0, 0,
    )
    if player is not None:
        payload += struct.pack("<Q", player)
    return record(SESSION_STATE, payload)


def mission_state() -> bytes:
    return record(MISSION_STATE, struct.pack("<QIBBHfQ", 0, 1, 0, 0, 0, 1.0, 1_000_000))


def lifecycle(player: int, class_reference: bool = False, object_type: int = 1) -> bytes:
    payload = struct.pack("<QQQBBI", player, 4 if class_reference else 0, 1_000_000, object_type, 1, 0)
    if class_reference:
        payload += struct.pack("<I", 1)
    return record(ENTITY_LIFECYCLE, payload)


def flight(player: int, presence: int = 0, changed: bool = False) -> bytes:
    payload = struct.pack("<QQQ", player, presence, 1_000_000)
    values = (10, 20, 30, 0.5, 0.5, 0.5, 0.5, 4, 5, 6, 0.1, 0.2, 0.3, 2, 3) if changed else \
             (0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    payload += struct.pack("<3f4f3f3ffI", *values)
    if presence & 1:
        payload += struct.pack("<3f", 0, 0, 0)
    return record(FLIGHT_STATE, payload)


def snapshot(records: list[bytes], required_manifest_id: int = 0) -> bytes:
    region = b"".join(records)
    return (
        struct.pack("<IHHI", 1, 0, 1, len(region)) + hashlib.sha256(region).digest()
        + struct.pack("<QIHH", 1_000_000, required_manifest_id, 1, len(records)) + region
    )


def datagram(message_type: int, payload: bytes, minor: int, session_id: int, flags: int = 0,
             frame_id: int = 0, mission_time_us: int = 0) -> bytes:
    message_crc = zlib.crc32(payload) & 0xffffffff
    values = (0x4c545346, 1, minor, message_type, flags, 68, len(payload), session_id,
              1, frame_id, mission_time_us, 2_000_000, 1, 0, 1, len(payload), 0, message_crc, 0)
    header = struct.pack("<IBBBBHHQIIqQIHHIIII", *values)
    assert len(header) == 68
    crc = zlib.crc32(header + payload) & 0xffffffff
    return header[:64] + struct.pack("<I", crc) + payload


def message_cases() -> dict[str, tuple[int, bytes, bytes, int, str]]:
    hello_v10 = (V1_VECTORS / "valid" / "messages" / "hello" / "hello.bin").read_bytes()
    welcome_v10 = (V1_VECTORS / "valid" / "messages" / "welcome" / "welcome.bin").read_bytes()
    hello_v11 = bytearray(hello_v10); hello_v11[18:20] = bytes((1, 1))
    welcome_v11 = bytearray(welcome_v10); welcome_v11[34] = 1
    welcome_rejected = bytearray(welcome_v10)
    welcome_rejected[32:36] = bytes((1, 0, 0, 0))
    welcome_rejected[36:60] = bytes(24)
    changed_records = [session_state(1, PLAYER_KINEMATICS), mission_state(), lifecycle(1), flight(1, changed=True)]
    changed_delta = struct.pack("<IIQHH", 1, 2, 1_000_001, len(changed_records), 0) + b"".join(changed_records)
    return_records = [session_state(1, PLAYER_KINEMATICS), mission_state(), lifecycle(1), flight(1)]
    return_delta = struct.pack("<IIQHH", 1, 3, 1_000_002, len(return_records), 0) + b"".join(return_records)
    session = 0x1122334455667788
    return {
        "hello-minor-one-only": (2, bytes(hello_v11), datagram(2, bytes(hello_v11), 0, 0), 1, "ACCEPT"),
        "welcome-accepted-minor-one": (3, bytes(welcome_v11), datagram(3, bytes(welcome_v11), 1, session, 2), 1, "ACCEPT"),
        "hello-minor-zero-only": (2, hello_v10, datagram(2, hello_v10, 0, 0), 0, "ACCEPT"),
        "welcome-unsupported-version": (3, bytes(welcome_rejected), datagram(3, bytes(welcome_rejected), 0, 0), 0, "UNSUPPORTED_VERSION"),
        "delta-player-kinematics-cumulative": (7, changed_delta,
            datagram(7, changed_delta, 1, session, frame_id=1, mission_time_us=1_000_001), 1, "ACCEPT"),
        "delta-player-return-baseline": (7, return_delta,
            datagram(7, return_delta, 1, session, frame_id=2, mission_time_us=1_000_002), 1, "ACCEPT"),
    }


def parse_a(data: bytes) -> dict[str, object]:
    if len(data) < 60:
        raise ValueError("TRUNCATED")
    snapshot_id, part_index, part_count, size = struct.unpack_from("<IHHI", data, 0)
    digest = data[12:44]
    sample, manifest_id, flags, count = struct.unpack_from("<QIHH", data, 44)
    region = data[60:]
    if len(region) != size or hashlib.sha256(region).digest() != digest:
        raise ValueError("TRANSACTION_MISMATCH")
    records = []
    offset = 0
    for _ in range(count):
        kind, version, rec_flags, length = struct.unpack_from("<HBBH", region, offset)
        start = offset + 6
        end = start + length
        if end > len(region):
            raise ValueError("TRUNCATED_RECORD")
        records.append((kind, region[start:end]))
        offset = end
    if offset != len(region):
        raise ValueError("TRAILING_RECORD_BYTES")
    return {"snapshot_id": snapshot_id, "part_index": part_index, "part_count": part_count,
            "sample": sample, "manifest_id": manifest_id, "flags": flags, "records": records}


class Cursor:
    def __init__(self, data: bytes): self.data, self.pos = data, 0
    def take(self, size: int) -> bytes:
        if self.pos + size > len(self.data): raise ValueError("TRUNCATED")
        value = self.data[self.pos:self.pos + size]; self.pos += size; return value
    def integer(self, size: int) -> int: return int.from_bytes(self.take(size), "little")


def parse_b(data: bytes) -> dict[str, object]:
    c = Cursor(data)
    snapshot_id, part_index, part_count, size = c.integer(4), c.integer(2), c.integer(2), c.integer(4)
    digest, sample, manifest_id, flags, count = c.take(32), c.integer(8), c.integer(4), c.integer(2), c.integer(2)
    region = c.take(size)
    if c.pos != len(data) or sha(region) != digest.hex(): raise ValueError("TRANSACTION_MISMATCH")
    r, records = Cursor(region), []
    for _ in range(count):
        kind, version, rec_flags, length = r.integer(2), r.integer(1), r.integer(1), r.integer(2)
        records.append((kind, r.take(length)))
    if r.pos != len(region): raise ValueError("TRAILING_RECORD_BYTES")
    return {"snapshot_id": snapshot_id, "part_index": part_index, "part_count": part_count,
            "sample": sample, "manifest_id": manifest_id, "flags": flags, "records": records}


def validate(decoded: dict[str, object], minor: int) -> str:
    records = decoded["records"]
    by_type = {kind: payload for kind, payload in records}
    if len(by_type) != len(records): return "DuplicateRecord"
    session = by_type.get(SESSION_STATE)
    if session is None or MISSION_STATE not in by_type: return "InvalidAbsence"
    presence = int.from_bytes(session[0:8], "little")
    coverage = int.from_bytes(session[40:48], "little")
    if session[24] != 0: return "InvalidStateTransition"
    if session[25] != 0: return "VisibilityViolation"
    player = int.from_bytes(session[64:72], "little") if presence & 1 else None
    if minor == 0 and coverage & PLAYER_KINEMATICS: return "ReservedFlag"
    if minor != 1 or not coverage & PLAYER_KINEMATICS: return "UNSUPPORTED_VERSION"
    if coverage == PLAYER_KINEMATICS:
        if decoded["manifest_id"] != 0: return "MANIFEST_FORBIDDEN"
        if SHIP_IDENTITY in by_type: return "InvalidAbsence"
        expected = {SESSION_STATE, MISSION_STATE} | ({ENTITY_LIFECYCLE, FLIGHT_STATE} if player else set())
        if set(by_type) != expected: return "InvalidAbsence"
        if player:
            life, fly = by_type[ENTITY_LIFECYCLE], by_type[FLIGHT_STATE]
            if int.from_bytes(life[0:8], "little") != player:
                return "InvalidAbsence"
            if int.from_bytes(life[8:16], "little") != 0 or life[24] != 1:
                return "InvalidStateTransition"
            if int.from_bytes(fly[0:8], "little") != player or int.from_bytes(fly[8:16], "little") != 0:
                return "InvalidAbsence"
        return "None"
    if coverage & CORE_SHIP:
        return "None" if decoded["manifest_id"] and CORE_RECORDS <= set(by_type) else "MissingManifest"
    return "INVALID_COVERAGE"


def cases() -> dict[str, tuple[bytes, int, str]]:
    player = 1
    promotion = [session_state(player, PLAYER_KINEMATICS | CORE_SHIP), mission_state(), lifecycle(player, True), flight(player)]
    fixture_names = {6: "ship_identity", 9: "damage_state", 10: "shield_state", 11: "subsystem_state",
                     12: "energy_state", 13: "propulsion_state"}
    promotion += [(V1_VECTORS / "valid" / "records" / fixture_names[kind] / f"{fixture_names[kind]}.bin").read_bytes()
                  for kind in sorted(CORE_RECORDS - {1, 2, 5, 7})]
    ship_identity = (V1_VECTORS / "valid" / "records" / "ship_identity" / "ship_identity.bin").read_bytes()
    base = [session_state(player, PLAYER_KINEMATICS), mission_state(), lifecycle(player), flight(player)]
    return {
        "minimal-no-player": (snapshot([session_state(None, PLAYER_KINEMATICS), mission_state()]), 1, "None"),
        "minimal-with-player": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(), lifecycle(player), flight(player)]), 1, "None"),
        "missing-mission": (snapshot([session_state(None, PLAYER_KINEMATICS)]), 1, "InvalidAbsence"),
        "missing-lifecycle": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(), flight(player)]), 1, "InvalidAbsence"),
        "missing-flight": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(), lifecycle(player)]), 1, "InvalidAbsence"),
        "non-solo-authority": (snapshot([session_state(player, PLAYER_KINEMATICS, authority=1), mission_state(),
                                          lifecycle(player), flight(player)]), 1, "InvalidStateTransition"),
        "non-cockpit-visibility": (snapshot([session_state(player, PLAYER_KINEMATICS, visibility=1), mission_state(),
                                              lifecycle(player), flight(player)]), 1, "VisibilityViolation"),
        "minor-zero-reserved-bit": (snapshot([session_state(None, PLAYER_KINEMATICS), mission_state()]), 0, "ReservedFlag"),
        "phase2-promotion-incomplete": (snapshot([session_state(player, PLAYER_KINEMATICS | CORE_SHIP), mission_state(), lifecycle(player), flight(player)], 1), 1, "MissingManifest"),
        "phase2-promotion": (snapshot(promotion, 1), 1, "None"),
        "duplicate-flight-record": (snapshot(base + [flight(player)]), 1, "DuplicateRecord"),
        "observed-player-id-mismatch": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(),
                                                   lifecycle(2), flight(2)]), 1, "InvalidAbsence"),
        "player-lifecycle-non-ship": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(),
                                                lifecycle(player, object_type=2), flight(player)]), 1,
                                      "InvalidStateTransition"),
        "unexpected-ship-identity": (snapshot(base + [ship_identity]), 1, "InvalidAbsence"),
        "flight-presence-not-covered": (snapshot([session_state(player, PLAYER_KINEMATICS), mission_state(),
                                                   lifecycle(player), flight(player, 1)]), 1,
                                        "InvalidAbsence"),
    }


def select_minor(producer: tuple[int, int], client: tuple[int, int]) -> int | None:
    low, high = max(producer[0], client[0]), min(producer[1], client[1])
    return high if low <= high else None


def delta_canonical(changed: bool, sequence: int, sample: int) -> bytes:
    template = json.loads((ROOT / "expected-v1.1/delta-player-kinematics-cumulative.json").read_text(encoding="utf-8"))
    template["fields"]["delta_sequence"] = sequence
    template["fields"]["producer_sample_time_us"] = str(sample)
    flight_fields = template["fields"]["records"][3]["fields"]
    flight_fields["position_world"] = [10.0, 20.0, 30.0] if changed else [0.0, 0.0, 0.0]
    flight_fields["orientation_local_to_world"] = [0.5, 0.5, 0.5, 0.5] if changed else [1.0, 0.0, 0.0, 0.0]
    flight_fields["velocity_world"] = [4.0, 5.0, 6.0] if changed else [0.0, 0.0, 0.0]
    flight_fields["rotational_velocity_local"] = [0.10000000149011612, 0.20000000298023224,
                                                   0.30000001192092896] if changed else [0.0, 0.0, 0.0]
    flight_fields["radius"] = 2.0 if changed else 0.0
    flight_fields["physics_mode_flags"] = 3 if changed else 0
    for item in template["fields"]["records"]: item["schema"] = "FSTL-1.1"
    template["messageFlags"] = 0
    return (json.dumps(template, indent=2, sort_keys=True) + "\n").encode()


def generated() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    entries = []
    for name, (binary, minor, expected) in cases().items():
        valid = expected == "None"
        metadata = {"schema": f"FSTL-1.{minor}", "name": name, "kind": "message-payload",
                    "messageType": 6, "headerVersion": f"1.{minor}", "inputFiles": [f"{name}.bin"],
                    "valid": valid, "expectedValidationError": ERROR_IDS[expected],
                    "expectedValidationErrorName": expected, "notes": f"FSTL 1.{minor} {name}",
                    "versionMinor": minor}
        if valid:
            canonical_path = f"expected-v1.1/{name}.json"
            metadata["expectedCanonicalJson"] = canonical_path
            files[f"../{canonical_path}"] = (ROOT / canonical_path).read_bytes()
        files[f"{name}/{name}.bin"] = binary
        files[f"{name}/{name}.json"] = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
    for name, (message_type, payload, encoded, minor, expected) in message_cases().items():
        metadata = {"schema": "FSTL-1.1", "name": name, "kind": "datagram",
                    "messageType": message_type, "versionMinor": minor,
                    "headerVersion": f"1.{encoded[5]}", "valid": True,
                    "expectedValidationError": 0, "expectedValidationErrorName": "None",
                    "notes": ("valid UnsupportedVersion rejection status" if expected == "UNSUPPORTED_VERSION"
                              else "valid FSTL 1.1 transport/message"),
                    "inputFiles": [f"{name}.bin"], "payloadFile": f"{name}.payload.bin"}
        canonical_path = f"expected-v1.1/{name}.json"
        metadata["expectedCanonicalJson"] = canonical_path
        if name == "delta-player-kinematics-cumulative":
            files[f"../{canonical_path}"] = delta_canonical(True, 2, 1_000_001)
        elif name == "delta-player-return-baseline":
            files[f"../{canonical_path}"] = delta_canonical(False, 3, 1_000_002)
        else:
            files[f"../{canonical_path}"] = (ROOT / canonical_path).read_bytes()
        files[f"{name}/{name}.bin"] = encoded
        files[f"{name}/{name}.payload.bin"] = payload
        files[f"{name}/{name}.json"] = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
    for name, data in sorted(files.items()):
        path = name[3:] if name.startswith("../") else f"vectors-v1.1/{name}"
        entries.append({"path": path, "sha256": sha(data)})
    frozen_count, frozen_tree = frozen_v10_contract()
    manifest = {"schema": "FSTL-1.1", "playerKinematics": PLAYER_KINEMATICS,
                "producerProfileMinorRange": [1, 1],
                "frozenV10ArtifactsManifest": "fstl-1.0-artifacts.manifest.json",
                "frozenV10ArtifactCount": frozen_count,
                "frozenV10ArtifactsTreeSha256": frozen_tree,
                "files": entries}
    files["../fstl-1.1-vectors.manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    return files


def main() -> int:
    parser = argparse.ArgumentParser(); mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true"); mode.add_argument("--check", action="store_true")
    parser.add_argument("--repo", type=Path, default=DEFAULT_REPO)
    args = parser.parse_args()
    configure_repo(args.repo)
    files = generated()
    if args.write:
        for relative, data in files.items():
            path = V11 / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    errors = []
    for relative, data in files.items():
        path = V11 / relative
        if not path.is_file() or path.read_bytes() != data: errors.append(f"drift: {path.relative_to(ROOT)}")
    for name, (binary, minor, expected) in cases().items():
        results = [validate(parse_a(binary), minor), validate(parse_b(binary), minor)]
        if results != [expected, expected]: errors.append(f"{name}: decoders={results}, expected={expected}")
    negotiations = [((1, 1), (1, 1), 1), ((1, 1), (0, 1), 1), ((1, 1), (0, 0), None)]
    for producer, client, expected in negotiations:
        if select_minor(producer, client) != expected:
            errors.append(f"negotiation: producer={producer}, client={client}, expected={expected}")
    schema_text = V11_SCHEMA.read_text(encoding="utf-8")
    schema = json.loads(schema_text)
    constants_text = (REPO / "code" / "telemetry" / "protocol" / "telemetry_protocol_constants.h").read_text(encoding="utf-8")
    if schema.get("wire_version") != "1.1":
        errors.append("contract: schema/fstl-v1.1.yaml does not declare FSTL 1.1")
    base_schema_path = ROOT / "schema" / "fstl-v1.yaml"
    base_schema = schema.get("base_schema", {})
    if (base_schema.get("path") != "test/telemetry/protocol/schema/fstl-v1.yaml" or
            base_schema.get("sha256") != sha(base_schema_path.read_bytes())):
        errors.append("contract: schema/fstl-v1.1.yaml base_schema identity drift")
    base_manifest = schema.get("base_artifact_manifest", {})
    frozen_count, frozen_tree = frozen_v10_contract()
    if (base_manifest.get("path") != "test/telemetry/protocol/fstl-1.0-artifacts.manifest.json" or
            base_manifest.get("sha256") != sha(V10_LEDGER.read_bytes()) or
            base_manifest.get("file_count") != frozen_count or
            base_manifest.get("tree_sha256") != frozen_tree):
        errors.append("contract: schema/fstl-v1.1.yaml base_artifact_manifest identity drift")
    for source in schema.get("normative_sources", []):
        source_path = REPO / source["path"]
        if not source_path.is_file() or sha(source_path.read_bytes()) != source["sha256"]:
            errors.append(f"contract: schema/fstl-v1.1.yaml source drift: {source['path']}")
    coverage_values = schema["numeric_registries"]["StateDomainCoverage"]["values"]
    if not any(value.get("name") == "PLAYER_KINEMATICS" and value.get("value") == 1024 for value in coverage_values):
        errors.append("contract: schema does not define PLAYER_KINEMATICS=0x400")
    if not re.search(r'PlayerKinematics[^\n]*?(?:1024|0x0*400)', constants_text, re.IGNORECASE):
        errors.append("contract: C++ constants do not define PlayerKinematics=0x400")
    if ("fstl_reference_" + "decoder") in Path(__file__).read_text(encoding="utf-8"):
        errors.append("independence: amendment generator depends on reference decoder")
    if errors:
        print("\n".join(errors)); return 1
    print(f"verified FSTL 1.1 amendment: snapshots={len(cases())}, messages={len(message_cases())}, "
          f"negotiations={len(negotiations)}, internal-oracles=2, "
          f"v1.0-artifacts={FROZEN_V10_ARTIFACT_COUNT}, v1.0-tree={FROZEN_V10_ARTIFACT_TREE_SHA256}")
    return 0


if __name__ == "__main__": raise SystemExit(main())

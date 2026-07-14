#!/usr/bin/env python3
"""Verify golden bytes and every frozen FSTL layout from fstl-v1.yaml.

The golden generator and independent decoder remain separate oracles. This
checker uses only the checked-in machine-readable schema plus the canonical
JSON projections: a changed field order, wire width, implicit length, nested
item layout, or record envelope therefore changes the reconstructed bytes and
fails the check. Valid goldens intentionally do not select every optional
record field, so deterministic non-semantic layout probes additionally encode
all record fields and every nested field occurrence. Their pinned fingerprints
make otherwise-unexercised wire drift fail without misrepresenting the probes
as semantically valid protocol fixtures.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_SCHEMA = REPO_ROOT / "test" / "telemetry" / "protocol" / "schema" / "fstl-v1.yaml"
DEFAULT_VECTORS = REPO_ROOT / "test" / "telemetry" / "protocol" / "vectors" / "valid"
DEFAULT_EXPECTED = REPO_ROOT / "test" / "telemetry" / "protocol" / "expected"


class VectorSchemaError(RuntimeError):
    pass


EXPECTED_MESSAGE_FIELD_COUNT = 192
EXPECTED_RECORD_FIELD_COUNT = 422
EXPECTED_STRUCTURED_TYPE_COUNT = 25
EXPECTED_STRUCTURED_FIELD_COUNT = 241
EXPECTED_EVENT_ITEM_FIELD_COUNT = 30
EXPECTED_LAYOUT_SHA256 = "d5e7ae20571bc0e08f1d123f7529fd6430466872ad0dd5cb22ea37b24128e0aa"
EXPECTED_ENCODED_PROBE_SHA256 = "ee45ad75728442145aa2689211f237868f095a39a2735b89ae5868c3dbe95438"


@dataclass(frozen=True)
class LayoutProbeSummary:
    message_fields: int
    record_fields: int
    structured_types: int
    structured_fields: int
    event_item_fields: int
    layout_sha256: str
    encoded_probe_sha256: str


@dataclass(frozen=True)
class VerificationSummary:
    message_fixtures: int
    record_fixtures: int
    reconstructed_bytes: int
    covered_message_fields: int
    total_message_fields: int
    covered_record_fields: int
    total_record_fields: int
    covered_structured_types: tuple[str, ...]
    layout_probes: LayoutProbeSummary
    wire_drift_self_tests: tuple[str, ...]


def integer(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise VectorSchemaError(f"expected integer-compatible value, got {value!r}")


def encoded_text(value: Any, encoding: str = "utf-8") -> bytes:
    if not isinstance(value, str):
        raise VectorSchemaError(f"expected text value, got {value!r}")
    return value.encode(encoding)


class SchemaEncoder:
    def __init__(self, schema: dict[str, Any]):
        self.schema = schema
        self.messages = {int(item["id"]): item for item in schema["message_types"]}
        self.records = {int(item["id"]): item for item in schema["record_types"]}
        self.structures = {item["name"]: item for item in schema["record_structured_types"]}
        self.aliases = {item["name"]: item["wire"] for item in schema["wire_aliases"]}
        self.registry_widths = {
            name: int(registry["width_bits"])
            for name, registry in schema["numeric_registries"].items()
        }
        self.covered_message_fields: set[tuple[int, str]] = set()
        self.covered_record_fields: set[tuple[int, str]] = set()
        self.covered_structure_fields: set[tuple[str, str, int]] = set()
        self.covered_structures: set[str] = set()

    @staticmethod
    def _canonical_key(name: str) -> str:
        # The canonical JSON projection predates the normative field rename;
        # it calls AssetEntryV1.relative_path "file_path". Wire bytes are the
        # same and the alias is deliberately confined to this test adapter.
        return "file_path" if name == "relative_path" else name

    def _implicit_value(self, name: str, values: dict[str, Any]) -> Any | None:
        length_sources = {
            "producer_name_length": "producer_name",
            "converter_id_length": "converter_id",
            "converter_version_length": "converter_version",
            "logical_name_length": "logical_name",
            "file_path_length": "file_path",
        }
        source = length_sources.get(name)
        if source is None or source not in values:
            return None
        return len(encoded_text(values[source], "ascii" if source == "file_path" else "utf-8"))

    def encode_message(self, canonical: dict[str, Any]) -> bytes:
        message_type = integer(canonical["messageType"])
        if message_type not in self.messages:
            raise VectorSchemaError(f"canonical message references unknown MessageType {message_type}")
        return self._encode_fields(
            self.messages[message_type]["fields"],
            canonical["fields"],
            owner_kind="message",
            owner_id=message_type,
            position_kind="offset",
        )

    def encode_record(self, canonical: dict[str, Any]) -> bytes:
        record_type = integer(canonical["recordType"])
        if record_type not in self.records:
            raise VectorSchemaError(f"canonical record references unknown RecordType {record_type}")
        record = self.records[record_type]
        payload = self._encode_fields(
            record["fields"],
            canonical["fields"],
            owner_kind="record",
            owner_id=record_type,
            position_kind=record["position_kind"],
        )
        expected_length = integer(canonical["recordLength"])
        if expected_length != len(payload):
            raise VectorSchemaError(
                f"RecordType {record_type} recordLength={expected_length}, schema reconstruction={len(payload)}"
            )
        return (
            record_type.to_bytes(2, "little")
            + integer(canonical["recordVersion"]).to_bytes(1, "little")
            + integer(canonical["recordFlags"]).to_bytes(1, "little")
            + len(payload).to_bytes(2, "little")
            + payload
        )

    def _encode_fields(
        self,
        fields: list[dict[str, Any]],
        values: dict[str, Any],
        *,
        owner_kind: str,
        owner_id: int | str,
        position_kind: str,
    ) -> bytes:
        output = bytearray()
        consumed_keys: set[str] = set()
        for field in fields:
            name = str(field["name"])
            key = self._canonical_key(name)
            implicit = False
            if key in values:
                value = values[key]
                consumed_keys.add(key)
            else:
                value = self._implicit_value(name, values)
                if value is None and name == "reserved" and "obligatoire" in str(field.get("rule", "")):
                    value = 0
                if value is None:
                    continue
                implicit = True
            if position_kind == "offset" and str(field.get("position", field.get("offset", ""))).isdigit():
                expected_offset = int(field.get("offset", field.get("position")))
                if len(output) != expected_offset:
                    raise VectorSchemaError(
                        f"{owner_kind} {owner_id} field {name} starts at {len(output)}, schema offset is {expected_offset}"
                    )
            encoded = self.encode_wire(str(field["wire"]), value, values, name)
            output.extend(encoded)
            if owner_kind == "message":
                self.covered_message_fields.add((int(owner_id), name))
            elif owner_kind == "record":
                self.covered_record_fields.add((int(owner_id), name))
            else:
                occurrence = int(field.get("occurrence", 1))
                self.covered_structure_fields.add((str(owner_id), name, occurrence))
                self.covered_structures.add(str(owner_id))
            if implicit:
                # The source value remains available to its following raw field.
                pass

        ignored_projection_keys = {"item_version", "item_size"}
        unexpected = sorted(set(values) - consumed_keys - ignored_projection_keys)
        if unexpected:
            raise VectorSchemaError(
                f"canonical {owner_kind} {owner_id} contains fields absent from schema: {unexpected}"
            )
        return bytes(output)

    def encode_wire(self, wire: str, value: Any, scope: dict[str, Any], field_name: str) -> bytes:
        if wire in ("u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64"):
            signed = wire.startswith("i")
            size = int(wire[1:]) // 8
            return integer(value).to_bytes(size, "little", signed=signed)
        if wire == "float32":
            number = float(value)
            if not math.isfinite(number):
                raise VectorSchemaError(f"{field_name} is not a finite binary32")
            if number == 0.0:
                number = 0.0
            return struct.pack("<f", number)
        if wire == "bool8":
            return integer(value).to_bytes(1, "little")
        if wire in ("entity_id", "asset_id", "duration_us", "sample_time_us"):
            return integer(value).to_bytes(8, "little")
        if wire in ("class_id", "weapon_class_id", "subsystem_id", "bank_id"):
            return integer(value).to_bytes(4, "little")
        if wire == "vec3f":
            return b"".join(self.encode_wire("float32", component, scope, field_name) for component in value)
        if wire == "quatf":
            return b"".join(self.encode_wire("float32", component, scope, field_name) for component in value)
        if wire == "mat3f":
            flat = [component for row in value for component in row] if value and isinstance(value[0], list) else value
            return b"".join(self.encode_wire("float32", component, scope, field_name) for component in flat)
        if wire == "rgba8":
            return bytes(integer(component) for component in value)
        if re.fullmatch(r"str<[0-9]+>", wire):
            payload = encoded_text(value)
            return len(payload).to_bytes(2, "little") + payload

        raw_string = re.fullmatch(r"(UTF-8|ASCII)(?:\[([a-zA-Z0-9_]+)\])?", wire)
        if raw_string:
            return encoded_text(value, "utf-8" if raw_string.group(1) == "UTF-8" else "ascii")

        array = re.fullmatch(r"(.+)\[([a-zA-Z0-9_]+)\]", wire)
        if array:
            item_wire, count_token = array.groups()
            count = int(count_token) if count_token.isdigit() else integer(scope[count_token])
            if item_wire == "bytes":
                encoded = bytes.fromhex(value) if isinstance(value, str) else bytes(value)
                if len(encoded) != count:
                    raise VectorSchemaError(f"{field_name} has {len(encoded)} bytes, expected {count}")
                return encoded
            if item_wire in self.structures:
                if len(value) != count:
                    raise VectorSchemaError(f"{field_name} has {len(value)} items, expected {count}")
                return b"".join(self._encode_structure(item_wire, item) for item in value)
            if len(value) != count:
                raise VectorSchemaError(f"{field_name} has {len(value)} items, expected {count}")
            return b"".join(self.encode_wire(item_wire, item, scope, field_name) for item in value)

        container = re.fullmatch(r"(v?list)<([A-Za-z][A-Za-z0-9_]*),([0-9]+)>", wire)
        if container:
            kind, item_wire, maximum = container.groups()
            if len(value) > int(maximum):
                raise VectorSchemaError(f"{field_name} exceeds {wire}")
            encoded = bytearray(len(value).to_bytes(2, "little"))
            if kind == "list" and value:
                item_payloads = [self._encode_container_item(item_wire, item) for item in value]
                if len({len(item) for item in item_payloads}) != 1:
                    raise VectorSchemaError(f"{field_name} list items do not share one fixed size")
                version = integer(value[0].get("item_version", 1)) if isinstance(value[0], dict) else 1
                encoded.extend(version.to_bytes(1, "little"))
                encoded.extend(len(item_payloads[0]).to_bytes(2, "little"))
                for payload in item_payloads:
                    encoded.extend(payload)
                return bytes(encoded)
            for item in value:
                payload = self._encode_container_item(item_wire, item)
                version = integer(item.get("item_version", 1)) if isinstance(item, dict) else 1
                declared_size = integer(item.get("item_size", len(payload))) if isinstance(item, dict) else len(payload)
                if declared_size != len(payload):
                    raise VectorSchemaError(
                        f"{field_name} item_size={declared_size}, schema reconstruction={len(payload)}"
                    )
                encoded.extend(version.to_bytes(1, "little"))
                encoded.extend(len(payload).to_bytes(2, "little"))
                encoded.extend(payload)
            return bytes(encoded)

        if wire in self.structures:
            return self._encode_structure(wire, value)
        if wire in self.registry_widths:
            return integer(value).to_bytes(self.registry_widths[wire] // 8, "little")
        if wire == "bytes" or wire.startswith("bytes["):
            if field_name == "records":
                return b"".join(self.encode_record(item) for item in value)
            if isinstance(value, list):
                if value:
                    raise VectorSchemaError(f"non-empty opaque list unsupported for {field_name}")
                return b""
            return bytes.fromhex(value) if isinstance(value, str) else bytes(value)
        raise VectorSchemaError(f"unsupported schema wire type {wire!r} for {field_name}")

    def _encode_container_item(self, item_wire: str, item: Any) -> bytes:
        if item_wire in self.structures:
            return self._encode_structure(item_wire, item)
        return self.encode_wire(item_wire, item, {}, item_wire)

    def _encode_structure(self, name: str, values: dict[str, Any]) -> bytes:
        structure = self.structures[name]
        return self._encode_fields(
            structure["fields"],
            values,
            owner_kind="structure",
            owner_id=name,
            position_kind=structure["position_kind"],
        )


def _selected_keys(item: dict[str, Any], keys: Sequence[str]) -> dict[str, Any]:
    return {key: item[key] for key in keys if key in item}


def schema_layout_projection(schema: dict[str, Any]) -> dict[str, Any]:
    """Return the frozen layout-only portion of the normative schema.

    Source locations are deliberately excluded so prose can move without
    changing the wire contract. Field rules, constraints, presence conditions,
    limits, aliases, positions, and wire types remain part of the fingerprint.
    """

    return {
        "message_types": [
            _selected_keys(item, ("id", "layout_name", "position_kind", "fields", "limits"))
            for item in schema["message_types"]
        ],
        "record_types": [
            _selected_keys(
                item,
                (
                    "id",
                    "layout_name",
                    "position_kind",
                    "fields",
                    "direct_nested_types",
                    "nested_types",
                ),
            )
            for item in schema["record_types"]
        ],
        "record_structured_types": [
            _selected_keys(
                item,
                (
                    "name",
                    "version",
                    "position_kind",
                    "fixed_prefix_bytes",
                    "fields",
                    "presence_bits",
                    "nested_types",
                ),
            )
            for item in schema["record_structured_types"]
        ],
        "wire_aliases": [
            _selected_keys(item, ("name", "wire", "rule")) for item in schema["wire_aliases"]
        ],
    }


def schema_layout_sha256(schema: dict[str, Any]) -> str:
    encoded = json.dumps(
        schema_layout_projection(schema),
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _minimal_structure_value(encoder: SchemaEncoder, name: str) -> dict[str, Any]:
    structure = encoder.structures[name]
    if structure["position_kind"] == "offset":
        values: dict[str, Any] = {}
        for field in structure["fields"]:
            value, _ = _probe_value(encoder, str(field["wire"]))
            values[encoder._canonical_key(str(field["name"]))] = value
        return values
    first_field = structure["fields"][0]
    value, _ = _probe_value(encoder, str(first_field["wire"]))
    return {encoder._canonical_key(str(first_field["name"])): value}


def _probe_value(encoder: SchemaEncoder, wire: str) -> tuple[Any, dict[str, Any]]:
    if wire in ("u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64"):
        return 1, {}
    if wire == "float32":
        return 1.25, {}
    if wire == "bool8":
        return 1, {}
    if wire in ("entity_id", "asset_id", "duration_us", "sample_time_us"):
        return 1, {}
    if wire in ("class_id", "weapon_class_id", "subsystem_id", "bank_id"):
        return 1, {}
    if wire == "vec3f":
        return [1.0, 2.0, 3.0], {}
    if wire == "quatf":
        return [1.0, 0.0, 0.0, 0.0], {}
    if wire == "mat3f":
        return [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0], {}
    if wire == "rgba8":
        return [1, 2, 3, 4], {}
    if re.fullmatch(r"str<[0-9]+>", wire):
        return "x", {}

    raw_string = re.fullmatch(r"(UTF-8|ASCII)(?:\[([a-zA-Z0-9_]+)\])?", wire)
    if raw_string:
        scope = {raw_string.group(2): 1} if raw_string.group(2) else {}
        return "x", scope

    array = re.fullmatch(r"(.+)\[([a-zA-Z0-9_]+)\]", wire)
    if array:
        item_wire, count_token = array.groups()
        if item_wire in encoder.structures:
            count = int(count_token) if count_token.isdigit() else 1
            scope = {} if count_token.isdigit() else {count_token: count}
            return [_minimal_structure_value(encoder, item_wire) for _ in range(count)], scope
        count = int(count_token) if count_token.isdigit() else 1
        scope = {} if count_token.isdigit() else {count_token: count}
        if item_wire == "bytes":
            return bytes((index % 251) + 1 for index in range(count)), scope
        item_value, _ = _probe_value(encoder, item_wire)
        return [item_value for _ in range(count)], scope

    container = re.fullmatch(r"(v?list)<([A-Za-z][A-Za-z0-9_]*),([0-9]+)>", wire)
    if container:
        _, item_wire, _ = container.groups()
        if item_wire in encoder.structures:
            return [_minimal_structure_value(encoder, item_wire)], {}
        item_value, _ = _probe_value(encoder, item_wire)
        return [item_value], {}

    if wire in encoder.structures:
        return _minimal_structure_value(encoder, wire), {}
    if wire in encoder.registry_widths:
        return 1, {}
    if wire == "bytes":
        return b"\xa5", {}
    raise VectorSchemaError(f"unsupported schema wire type {wire!r}")


def probe_wire(encoder: SchemaEncoder, wire: str, label: str) -> bytes:
    try:
        value, scope = _probe_value(encoder, wire)
        return encoder.encode_wire(wire, value, scope, label)
    except (KeyError, OverflowError, TypeError, ValueError, VectorSchemaError) as exc:
        raise VectorSchemaError(f"layout probe failed for {label}:{wire}: {exc}") from exc


def _update_probe_digest(digest: Any, label: str, wire: str, encoded: bytes) -> None:
    for value in (label.encode("utf-8"), wire.encode("utf-8"), encoded):
        digest.update(len(value).to_bytes(4, "little"))
        digest.update(value)


def _owner_probe_values(encoder: SchemaEncoder, fields: list[dict[str, Any]]) -> dict[str, Any]:
    occurrences: dict[str, int] = {}
    for field in fields:
        name = encoder._canonical_key(str(field["name"]))
        occurrences[name] = occurrences.get(name, 0) + 1

    values: dict[str, Any] = {}
    for field in fields:
        name = encoder._canonical_key(str(field["name"]))
        # Repeated reserved slots can have different wire widths. They are
        # covered by the per-occurrence probes below instead of one ambiguous
        # canonical JSON key.
        if occurrences[name] != 1:
            continue
        value, _ = _probe_value(encoder, str(field["wire"]))
        values[name] = value
    return values


def verify_layout_probes(schema: dict[str, Any], encoder: SchemaEncoder) -> LayoutProbeSummary:
    layout_sha256 = schema_layout_sha256(schema)
    if layout_sha256 != EXPECTED_LAYOUT_SHA256:
        raise VectorSchemaError(
            f"frozen schema layout drift: {layout_sha256} != {EXPECTED_LAYOUT_SHA256}"
        )

    encoded_probe_digest = hashlib.sha256()
    message_fields = 0
    for message in schema["message_types"]:
        for field in message["fields"]:
            label = f"MessageType {message['id']} {field['name']}"
            wire = str(field["wire"])
            encoded = probe_wire(encoder, wire, label)
            _update_probe_digest(encoded_probe_digest, label, wire, encoded)
            message_fields += 1

    record_fields = 0
    for record in schema["record_types"]:
        aggregate_label = f"RecordType {record['id']} aggregate"
        aggregate = encoder._encode_fields(
            record["fields"],
            _owner_probe_values(encoder, record["fields"]),
            owner_kind="record",
            owner_id=int(record["id"]),
            position_kind=str(record["position_kind"]),
        )
        _update_probe_digest(encoded_probe_digest, aggregate_label, "record-payload", aggregate)
        for field in record["fields"]:
            label = f"RecordType {record['id']} {field['name']}"
            wire = str(field["wire"])
            encoded = probe_wire(encoder, wire, label)
            _update_probe_digest(encoded_probe_digest, label, wire, encoded)
            record_fields += 1
    if len(encoder.covered_record_fields) != record_fields:
        raise VectorSchemaError(
            "aggregate record probes do not exercise every field: "
            f"{len(encoder.covered_record_fields)}/{record_fields}"
        )

    structured_fields = 0
    event_item_fields = 0
    structured_types = schema["record_structured_types"]
    for structure in structured_types:
        aggregate_label = f"{structure['name']} aggregate"
        aggregate = encoder._encode_structure(
            str(structure["name"]),
            _owner_probe_values(encoder, structure["fields"]),
        )
        _update_probe_digest(encoded_probe_digest, aggregate_label, "structured-payload", aggregate)
        for field in structure["fields"]:
            occurrence = int(field.get("occurrence", 1))
            label = f"{structure['name']} {field['name']}#{occurrence}"
            wire = str(field["wire"])
            encoded = probe_wire(encoder, wire, label)
            _update_probe_digest(encoded_probe_digest, label, wire, encoded)
            encoder.covered_structure_fields.add(
                (str(structure["name"]), str(field["name"]), occurrence)
            )
            structured_fields += 1
            if structure["name"] == "EventItemV1":
                event_item_fields += 1
    expected_structures = {str(item["name"]) for item in structured_types}
    if encoder.covered_structures != expected_structures:
        raise VectorSchemaError(
            "aggregate nested probes do not exercise every structured type: "
            f"missing={sorted(expected_structures - encoder.covered_structures)}"
        )
    if len(encoder.covered_structure_fields) != structured_fields:
        raise VectorSchemaError(
            "nested field/occurrence probes are not unique and exhaustive: "
            f"{len(encoder.covered_structure_fields)}/{structured_fields}"
        )

    observed = (
        message_fields,
        record_fields,
        len(structured_types),
        structured_fields,
        event_item_fields,
    )
    expected = (
        EXPECTED_MESSAGE_FIELD_COUNT,
        EXPECTED_RECORD_FIELD_COUNT,
        EXPECTED_STRUCTURED_TYPE_COUNT,
        EXPECTED_STRUCTURED_FIELD_COUNT,
        EXPECTED_EVENT_ITEM_FIELD_COUNT,
    )
    if observed != expected:
        raise VectorSchemaError(f"schema layout probe coverage drift: {observed} != {expected}")
    encoded_probe_sha256 = encoded_probe_digest.hexdigest()
    if encoded_probe_sha256 != EXPECTED_ENCODED_PROBE_SHA256:
        raise VectorSchemaError(
            "frozen encoded layout probe drift: "
            f"{encoded_probe_sha256} != {EXPECTED_ENCODED_PROBE_SHA256}"
        )

    return LayoutProbeSummary(
        message_fields=message_fields,
        record_fields=record_fields,
        structured_types=len(structured_types),
        structured_fields=structured_fields,
        event_item_fields=event_item_fields,
        layout_sha256=layout_sha256,
        encoded_probe_sha256=encoded_probe_sha256,
    )


def run_wire_drift_self_tests(schema: dict[str, Any]) -> tuple[str, ...]:
    passed: list[str] = []

    def expect_fingerprint_drift(label: str, mutate: Any) -> None:
        candidate = json.loads(json.dumps(schema))
        mutate(candidate)
        if schema_layout_sha256(candidate) == EXPECTED_LAYOUT_SHA256:
            raise VectorSchemaError(f"wire-drift self-test unexpectedly passed: {label}")
        try:
            verify_layout_probes(candidate, SchemaEncoder(candidate))
        except VectorSchemaError:
            pass
        else:
            raise VectorSchemaError(f"wire-drift verifier unexpectedly passed: {label}")
        passed.append(label)

    expect_fingerprint_drift(
        "uncovered-record-wire",
        lambda candidate: next(
            field
            for field in candidate["record_types"][2]["fields"]
            if field["name"] == "inertia_tensor"
        ).__setitem__("wire", "u8"),
    )
    expect_fingerprint_drift(
        "uncovered-nested-wire",
        lambda candidate: next(
            field
            for field in next(
                item
                for item in candidate["record_structured_types"]
                if item["name"] == "ClassMotionV1"
            )["fields"]
            if field["name"] == "max_vel"
        ).__setitem__("wire", "u8"),
    )
    expect_fingerprint_drift(
        "event-item-wire",
        lambda candidate: next(
            field
            for field in next(
                item
                for item in candidate["record_structured_types"]
                if item["name"] == "EventItemV1"
            )["fields"]
            if field["name"] == "actor_entity_id"
        ).__setitem__("wire", "u32"),
    )

    broken = json.loads(json.dumps(schema))
    next(
        field
        for field in next(
            item
            for item in broken["record_structured_types"]
            if item["name"] == "ClassMotionV1"
        )["fields"]
        if field["name"] == "max_vel"
    )["wire"] = "BROKEN_WIRE"
    try:
        broken_encoder = SchemaEncoder(broken)
        probe_wire(broken_encoder, "BROKEN_WIRE", "ClassMotionV1 max_vel#1")
    except VectorSchemaError:
        passed.append("unresolved-wire")
    else:
        raise VectorSchemaError("wire-drift self-test unexpectedly passed: unresolved-wire")
    return tuple(passed)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise VectorSchemaError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise VectorSchemaError(f"JSON root must be an object: {path}")
    return value


def verify_schema_vectors(
    schema: dict[str, Any], vectors_root: Path = DEFAULT_VECTORS, expected_root: Path = DEFAULT_EXPECTED
) -> VerificationSummary:
    encoder = SchemaEncoder(schema)
    layout_probes = verify_layout_probes(schema, SchemaEncoder(schema))
    wire_drift_self_tests = run_wire_drift_self_tests(schema)
    message_count = 0
    record_count = 0
    reconstructed_bytes = 0
    observed_message_types: set[int] = set()
    observed_record_types: set[int] = set()

    for kind in ("messages", "records"):
        directory = vectors_root / kind
        for metadata_path in sorted(directory.glob("*/*.json")):
            metadata = load_json(metadata_path)
            if metadata.get("valid") is not True:
                continue
            input_files = metadata.get("inputFiles")
            if not isinstance(input_files, list) or len(input_files) != 1:
                raise VectorSchemaError(f"valid fixture must have exactly one input file: {metadata_path}")
            binary_path = metadata_path.parent / str(input_files[0])
            canonical_path = expected_root / str(metadata["expectedCanonicalJson"])
            canonical = load_json(canonical_path)
            actual = binary_path.read_bytes()
            if kind == "messages":
                reconstructed = encoder.encode_message(canonical)
                observed_message_types.add(integer(canonical["messageType"]))
                message_count += 1
            else:
                reconstructed = encoder.encode_record(canonical)
                observed_record_types.add(integer(canonical["recordType"]))
                record_count += 1
            if reconstructed != actual:
                mismatch = next(
                    (index for index, pair in enumerate(zip(reconstructed, actual)) if pair[0] != pair[1]),
                    min(len(reconstructed), len(actual)),
                )
                raise VectorSchemaError(
                    f"schema reconstruction differs at byte {mismatch}: {binary_path} "
                    f"(schema={len(reconstructed)} bytes, fixture={len(actual)} bytes)"
                )
            reconstructed_bytes += len(actual)

    if observed_message_types != set(range(1, 21)):
        raise VectorSchemaError(f"message fixture coverage drift: {sorted(observed_message_types)}")
    if observed_record_types != set(range(1, 29)):
        raise VectorSchemaError(f"record fixture coverage drift: {sorted(observed_record_types)}")

    total_message_fields = sum(len(item["fields"]) for item in schema["message_types"])
    if len(encoder.covered_message_fields) != total_message_fields:
        missing = [
            f"{item['id']}:{field['name']}"
            for item in schema["message_types"]
            for field in item["fields"]
            if (int(item["id"]), str(field["name"])) not in encoder.covered_message_fields
        ]
        raise VectorSchemaError(f"golden messages do not exercise every payload field: {missing}")
    for required_nested in ("AssetEntryV1", "EventItemV1"):
        if required_nested not in encoder.covered_structures:
            raise VectorSchemaError(f"golden records do not exercise {required_nested}")

    return VerificationSummary(
        message_fixtures=message_count,
        record_fixtures=record_count,
        reconstructed_bytes=reconstructed_bytes,
        covered_message_fields=len(encoder.covered_message_fields),
        total_message_fields=total_message_fields,
        covered_record_fields=len(encoder.covered_record_fields),
        total_record_fields=sum(len(item["fields"]) for item in schema["record_types"]),
        covered_structured_types=tuple(sorted(encoder.covered_structures)),
        layout_probes=layout_probes,
        wire_drift_self_tests=wire_drift_self_tests,
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--vectors", type=Path, default=DEFAULT_VECTORS)
    parser.add_argument("--expected", type=Path, default=DEFAULT_EXPECTED)
    parser.add_argument("--check", action="store_true", help="accepted for symmetry; verification is the default")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        schema = load_json(args.schema.resolve())
        summary = verify_schema_vectors(schema, args.vectors.resolve(), args.expected.resolve())
    except (OSError, VectorSchemaError) as exc:
        print(f"FSTL schema/vector verification failed: {exc}", file=sys.stderr)
        return 1
    print(
        "FSTL schema/vector bytes verified: "
        f"{summary.message_fixtures} messages, {summary.record_fixtures} records, "
        f"{summary.reconstructed_bytes} bytes; message fields "
        f"{summary.covered_message_fields}/{summary.total_message_fields}, record fields "
        f"{summary.covered_record_fields}/{summary.total_record_fields}; golden nested "
        f"{', '.join(summary.covered_structured_types)}. Schema layout probes: "
        f"messages {summary.layout_probes.message_fields}/{EXPECTED_MESSAGE_FIELD_COUNT}, "
        f"records {summary.layout_probes.record_fields}/{EXPECTED_RECORD_FIELD_COUNT}, "
        f"nested {summary.layout_probes.structured_types}/{EXPECTED_STRUCTURED_TYPE_COUNT} types and "
        f"{summary.layout_probes.structured_fields}/{EXPECTED_STRUCTURED_FIELD_COUNT} fields, "
        f"EventItemV1 {summary.layout_probes.event_item_fields}/{EXPECTED_EVENT_ITEM_FIELD_COUNT}; "
        f"layout sha256 {summary.layout_probes.layout_sha256}, encoded probes sha256 "
        f"{summary.layout_probes.encoded_probe_sha256}; wire-drift self-tests "
        f"{', '.join(summary.wire_drift_self_tests)}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verify frozen FSTL 1.0 and generate the additive FSTL 1.1 registry.

The checked-in ``fstl-v1.yaml`` deliberately uses JSON syntax. JSON is a
subset of YAML 1.2, so the artifact remains consumable without adding a YAML
dependency to the repository. This tool itself uses only the Python standard
library.

The FSTL 1.0 schema is a byte-frozen input and ordinary ``--write`` mode can
never refresh it. FSTL 1.1 is deterministically derived from that frozen base
and carries non-normative product-document provenance.
"""

from __future__ import annotations

import argparse
import ast
import difflib
import hashlib
import json
import math
import re
import subprocess
import sys
import tempfile
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


class SchemaError(RuntimeError):
    """Raised when normative sources are missing or internally inconsistent."""


@dataclass(frozen=True)
class MarkdownTable:
    headers: tuple[str, ...]
    rows: tuple[tuple[str, ...], ...]


REPO_ROOT = Path(__file__).resolve().parents[4]
PHASE_DIR = REPO_ROOT / "documentation" / "analysis" / "specs" / "0-Contrat-de-protocole"
PHASE1_DIR = REPO_ROOT / "documentation" / "analysis" / "specs" / "1-Squelette-et-premier-flux"
SCHEMA_V1_0_PATH = REPO_ROOT / "test" / "telemetry" / "protocol" / "schema" / "fstl-v1.yaml"
SCHEMA_V1_1_PATH = REPO_ROOT / "test" / "telemetry" / "protocol" / "schema" / "fstl-v1.1.yaml"
SCHEMA_PATH = SCHEMA_V1_0_PATH
FROZEN_LEDGER_PATH = REPO_ROOT / "test" / "telemetry" / "protocol" / "fstl-1.0-artifacts.manifest.json"
CPP_CONSTANTS_PATH = REPO_ROOT / "code" / "telemetry" / "protocol" / "telemetry_protocol_constants.h"
SCHEMA_VECTOR_CHECKER_PATH = Path(__file__).resolve().with_name("verify_schema_vectors.py")

FROZEN_SCHEMA_V1_0_BYTES = 499_786
FROZEN_SCHEMA_V1_0_SHA256 = "1d89c4a95a121c178bf85570cd616568fd939942b8d053835069b2d7d6a1f0d4"
FROZEN_ARTIFACT_COUNT_V1_0 = 438
FROZEN_ARTIFACT_TREE_SHA256_V1_0 = "9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d"
SOURCE_NAMES = (
    "01-cadre-normatif-et-perimetre.md",
    "02-format-filaire-et-registres.md",
    "03-session-horloges-fiabilite.md",
    "04-modele-de-donnees-v1.md",
    "05-capabilities-et-vues-specialisees.md",
    "06-validation-securite-et-conformite.md",
    "07-livraison-et-tracabilite.md",
)

PHASE1_SOURCE_NAMES = (
    "01-cadre-normatif-et-perimetre.md",
    "02-architecture-contrats-et-interfaces.md",
    "03-flux-cycle-de-vie-et-concurrence.md",
    "04-modele-de-donnees-et-regles-metier.md",
    "05-integration-configuration-et-observabilite.md",
    "06-validation-securite-et-conformite.md",
    "07-livraison-et-tracabilite.md",
)


def read_sources() -> dict[str, str]:
    sources: dict[str, str] = {}
    for name in SOURCE_NAMES:
        path = PHASE_DIR / name
        try:
            sources[name] = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise SchemaError(f"cannot read normative source {path}: {exc}") from exc
    return sources


def strip_inline_markdown(value: str) -> str:
    value = value.strip()
    value = re.sub(r"!\[([^]]*)\]\([^)]+\)", r"\1", value)
    value = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", value)
    value = value.replace("`", "").replace("**", "").replace("__", "")
    value = value.replace(r"\|", "|")
    return value.strip()


def keyify(value: str) -> str:
    value = strip_inline_markdown(value)
    value = unicodedata.normalize("NFKD", value)
    value = "".join(ch for ch in value if not unicodedata.combining(ch))
    value = value.lower()
    return re.sub(r"[^a-z0-9]+", "_", value).strip("_")


def normalize_symbol(value: str) -> str:
    return "".join(ch.lower() for ch in strip_inline_markdown(value) if ch.isalnum())


def upper_snake(value: str) -> str:
    value = strip_inline_markdown(value)
    value = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", value)
    value = re.sub(r"[^A-Za-z0-9]+", "_", value)
    return value.strip("_").upper()


def split_table_row(line: str) -> tuple[str, ...]:
    stripped = line.strip()
    if not (stripped.startswith("|") and stripped.endswith("|")):
        raise SchemaError(f"not a Markdown table row: {line!r}")
    body = stripped[1:-1]
    return tuple(part.strip() for part in re.split(r"(?<!\\)\|", body))


def is_table_separator(line: str) -> bool:
    try:
        cells = split_table_row(line)
    except SchemaError:
        return False
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell.strip()) for cell in cells)


def parse_tables(text: str) -> tuple[MarkdownTable, ...]:
    lines = text.splitlines()
    result: list[MarkdownTable] = []
    index = 0
    while index + 1 < len(lines):
        if not lines[index].lstrip().startswith("|") or not is_table_separator(lines[index + 1]):
            index += 1
            continue

        headers = split_table_row(lines[index])
        rows: list[tuple[str, ...]] = []
        index += 2
        while index < len(lines) and lines[index].lstrip().startswith("|"):
            cells = split_table_row(lines[index])
            if len(cells) != len(headers):
                raise SchemaError(
                    f"table row has {len(cells)} cells but header has {len(headers)}: {lines[index]!r}"
                )
            rows.append(cells)
            index += 1
        result.append(MarkdownTable(headers=headers, rows=tuple(rows)))
    return tuple(result)


def extract_section(text: str, heading_pattern: str) -> str:
    lines = text.splitlines()
    matcher = re.compile(heading_pattern)
    start = -1
    level = 0
    for index, line in enumerate(lines):
        if matcher.fullmatch(line):
            start = index
            level = len(line) - len(line.lstrip("#"))
            break
    if start < 0:
        raise SchemaError(f"normative heading not found: {heading_pattern}")

    end = len(lines)
    for index in range(start + 1, len(lines)):
        match = re.match(r"^(#+)\s", lines[index])
        if match and len(match.group(1)) <= level:
            end = index
            break
    return "\n".join(lines[start:end])


def table_header_keys(table: MarkdownTable) -> tuple[str, ...]:
    return tuple(keyify(header) for header in table.headers)


def find_table(text: str, required_headers: Sequence[str]) -> MarkdownTable:
    required = tuple(keyify(header) for header in required_headers)
    for table in parse_tables(text):
        keys = table_header_keys(table)
        if all(any(wanted == key or wanted in key for key in keys) for wanted in required):
            return table
    raise SchemaError(f"table with headers {required_headers!r} not found")


def row_dict(table: MarkdownTable, row: Sequence[str]) -> dict[str, str]:
    return dict(zip(table_header_keys(table), row))


def cell(row: dict[str, str], *names: str) -> str:
    keys = tuple(keyify(name) for name in names)
    for name in keys:
        if name in row:
            return row[name]
    for name in keys:
        for key, value in row.items():
            if name in key:
                return value
    raise SchemaError(f"missing expected column {names!r} in row with columns {tuple(row)}")


def optional_cell(row: dict[str, str], *names: str) -> str:
    try:
        return cell(row, *names)
    except SchemaError:
        return ""


def parse_exact_int(value: str) -> int:
    cleaned = strip_inline_markdown(value)
    compact = re.sub(r"[\s\u00a0\u202f]", "", cleaned)
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", compact):
        return int(compact, 16)
    if re.fullmatch(r"[0-9]+", compact):
        return int(compact, 10)
    raise SchemaError(f"expected an exact integer, got {value!r}")


def first_numeric_value(value: str) -> int | None:
    cleaned = strip_inline_markdown(value)
    hex_match = re.search(r"0[xX][0-9a-fA-F]+", cleaned)
    if hex_match:
        return int(hex_match.group(0), 16)
    number_match = re.search(r"[0-9](?:[0-9\s\u00a0\u202f]*[0-9])?", cleaned)
    if not number_match:
        return None
    return int(re.sub(r"[\s\u00a0\u202f]", "", number_match.group(0)))


def require_unique(items: Iterable[dict[str, object]], field: str, registry: str) -> None:
    seen: dict[object, int] = {}
    for index, item in enumerate(items):
        value = item[field]
        if value in seen:
            raise SchemaError(
                f"{registry} collision for {field}={value!r} at entries {seen[value]} and {index}"
            )
        seen[value] = index


def require_unique_normalized_names(
    items: Iterable[dict[str, object]], field: str, registry: str
) -> None:
    seen: dict[str, str] = {}
    for item in items:
        original = str(item[field])
        normalized = normalize_symbol(original)
        if normalized in seen:
            raise SchemaError(
                f"{registry} normalized-name collision: {seen[normalized]!r} and {original!r}"
            )
        seen[normalized] = original


def require_contiguous(items: Sequence[dict[str, object]], field: str, start: int, end: int, registry: str) -> None:
    observed = sorted(int(item[field]) for item in items)
    expected = list(range(start, end + 1))
    if observed != expected:
        raise SchemaError(f"{registry} IDs are not exactly {start}..{end}: {observed}")


def table_after(text: str, marker_pattern: str, required_headers: Sequence[str]) -> MarkdownTable:
    marker = re.search(marker_pattern, text, re.IGNORECASE)
    if marker is None:
        raise SchemaError(f"normative marker not found: {marker_pattern}")
    return find_table(text[marker.end():], required_headers)


def numeric_assignments(value: str) -> list[dict[str, object]]:
    """Parse the normative ``NAME=value`` notation without inventing values."""

    cleaned = strip_inline_markdown(value)
    result: list[dict[str, object]] = []
    for name, raw_value in re.findall(
        r"\b([A-Z][A-Z0-9_]*)\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)\b", cleaned
    ):
        result.append({"name": name, "value": int(raw_value, 0)})
    require_unique(result, "name", "numeric assignment")
    require_unique(result, "value", "numeric assignment")
    return result


def backtick_number_names(value: str) -> list[dict[str, object]]:
    result = [
        {"name": upper_snake(name), "value": int(number)}
        for number, name in re.findall(r"`([0-9]+)\s+([A-Za-z][A-Za-z0-9_]*)`", value)
    ]
    require_unique(result, "name", "number/name prose")
    require_unique(result, "value", "number/name prose")
    return result


def named_bits(value: str) -> list[dict[str, object]]:
    pairs: list[tuple[str, str]] = []
    pairs.extend(re.findall(r"\bbit\s+([0-9]+)[^`\n;]*`([A-Z][A-Z0-9_]*)`", value))
    # The French prose also uses ``NAME=bit0`` for compact item definitions.
    pairs.extend((bit, name) for name, bit in re.findall(r"\b([A-Z][A-Z0-9_]*)\s*=\s*bit\s*([0-9]+)", value))
    result = [
        {"name": upper_snake(name), "bit": int(bit), "value": 1 << int(bit)}
        for bit, name in pairs
        if "reserve" not in keyify(name)
    ]
    # Keep the first occurrence where a sentence repeats a bit semantically.
    deduplicated: list[dict[str, object]] = []
    seen: set[tuple[str, int]] = set()
    for item in result:
        marker = (str(item["name"]), int(item["bit"]))
        if marker not in seen:
            deduplicated.append(item)
            seen.add(marker)
    return deduplicated


def registry_source(document: str, section: str) -> dict[str, str]:
    return {
        "document": f"documentation/analysis/specs/0-Contrat-de-protocole/{document}",
        "section": section,
    }


def make_enum_registry(
    name: str,
    width_bits: int,
    values: Sequence[dict[str, object]],
    source: dict[str, str],
    *,
    unknown_policy: str = "reject",
    unknown_handling: str | None = None,
    reserved_ranges: Sequence[Sequence[int]] = (),
    notes: Sequence[str] = (),
) -> dict[str, object]:
    copied = [dict(item) for item in values]
    if not copied:
        raise SchemaError(f"numeric enum {name} has no values")
    require_unique(copied, "name", name)
    require_unique_normalized_names(copied, "name", name)
    require_unique(copied, "value", name)
    maximum = (1 << width_bits) - 1
    for item in copied:
        value = int(item["value"])
        if value < 0 or value > maximum:
            raise SchemaError(f"{name}.{item['name']}={value} does not fit u{width_bits}")
    result: dict[str, object] = {
        "kind": "enum",
        "width_bits": width_bits,
        "values": copied,
        "unknown_policy": unknown_policy,
        "reserved": {
            "policy": unknown_policy,
            "ranges": [list(pair) for pair in reserved_ranges],
        },
        "source": source,
    }
    if unknown_handling is not None:
        result["unknown_handling"] = unknown_handling
    if notes:
        result["notes"] = list(notes)
    return result


def make_bitmask_registry(
    name: str,
    width_bits: int,
    values: Sequence[dict[str, object]],
    source: dict[str, str],
    *,
    extensible: bool = False,
    allow_empty: bool = False,
    notes: Sequence[str] = (),
) -> dict[str, object]:
    copied: list[dict[str, object]] = []
    for raw_item in values:
        item = dict(raw_item)
        value = int(item["value"])
        if value <= 0 or value & (value - 1):
            raise SchemaError(f"{name}.{item['name']}={value:#x} is not a single-bit mask")
        bit = value.bit_length() - 1
        if "bit" in item and int(item["bit"]) != bit:
            raise SchemaError(f"{name}.{item['name']} bit/mask mismatch")
        item["bit"] = bit
        copied.append(item)
    if not copied and not allow_empty:
        raise SchemaError(f"numeric bitmask {name} has no values")
    require_unique(copied, "name", name)
    require_unique_normalized_names(copied, "name", name)
    require_unique(copied, "value", name)
    require_unique(copied, "bit", name)
    maximum = (1 << width_bits) - 1
    known_mask = 0
    for item in copied:
        value = int(item["value"])
        if value > maximum:
            raise SchemaError(f"{name}.{item['name']}={value:#x} does not fit u{width_bits}")
        known_mask |= value
    unknown_policy = "ignore" if extensible else "reject"
    reserved_handling = "ignore_on_read_emit_zero" if extensible else "reject_nonzero"
    result: dict[str, object] = {
        "kind": "bitmask",
        "width_bits": width_bits,
        "values": copied,
        "unknown_policy": unknown_policy,
        "reserved": {
            "policy": reserved_handling,
            "known_mask": known_mask,
            "reserved_mask": maximum ^ known_mask,
        },
        "source": source,
    }
    if notes:
        result["notes"] = list(notes)
    return result


def values_from_value_name_table(
    table: MarkdownTable,
    *,
    value_header: str = "Valeur",
    name_header: str = "Nom",
    metadata_headers: Sequence[str] = (),
) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        raw_name = strip_inline_markdown(cell(row, name_header))
        raw_name = raw_name.split(",", 1)[0].strip()
        if not raw_name or "reserve" in keyify(raw_name):
            continue
        try:
            value = parse_exact_int(cell(row, value_header))
        except SchemaError:
            continue
        item: dict[str, object] = {"name": upper_snake(raw_name), "value": value}
        for header in metadata_headers:
            item[keyify(header)] = strip_inline_markdown(cell(row, header))
        result.append(item)
    return result


def values_from_bit_table(
    table: MarkdownTable,
    *,
    bit_header: str = "Bit",
    value_header: str = "Valeur",
    name_header: str = "Nom",
    metadata_headers: Sequence[str] = (),
) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        raw_name = strip_inline_markdown(cell(row, name_header))
        raw_name = raw_name.split(",", 1)[0].strip()
        if not raw_name or "reserve" in keyify(raw_name):
            continue
        try:
            bit = parse_exact_int(cell(row, bit_header))
            value = parse_exact_int(cell(row, value_header))
        except SchemaError:
            continue
        item: dict[str, object] = {"name": upper_snake(raw_name), "value": value, "bit": bit}
        for header in metadata_headers:
            item[keyify(header)] = strip_inline_markdown(cell(row, header))
        result.append(item)
    return result


def assert_same_registry_values(
    name: str, left: Sequence[dict[str, object]], right: Sequence[dict[str, object]], context: str
) -> None:
    def canonical(values: Sequence[dict[str, object]]) -> list[tuple[str, int]]:
        return sorted((normalize_symbol(str(item["name"])), int(item["value"])) for item in values)

    if canonical(left) != canonical(right):
        raise SchemaError(f"{name} drifts between normative definitions ({context})")


def canonical_direction(value: str) -> str:
    normalized = keyify(value)
    if "bidirectionnel" in normalized:
        return "bidirectional"
    client = normalized.find("client")
    producer = normalized.find("producteur")
    if client >= 0 and producer >= 0:
        return "client_to_producer" if client < producer else "producer_to_client"
    return normalized


def canonical_delivery_tags(value: str) -> set[str]:
    normalized = keyify(value)
    tags: set[str] = set()
    if "non_fiable" in normalized:
        tags.add("unreliable")
    elif "fiable" in normalized:
        tags.add("reliable")
    if "rempla" in normalized:
        tags.add("replaceable")
    if "period" in normalized:
        tags.add("periodic")
    if "idempotent" in normalized:
        tags.add("idempotent")
    if "idr" in normalized or "selective" in normalized:
        tags.add("idr_recovery")
    return tags


def container_symbols(value: str) -> list[str]:
    known = ("MANIFEST", "FULL_SNAPSHOT", "DELTA", "EVENT_BATCH")
    normalized = upper_snake(value)
    return [symbol for symbol in known if symbol in normalized]


def parse_message_types(doc02: str, doc03: str, doc05: str) -> tuple[dict[str, object], list[dict[str, object]]]:
    section = extract_section(doc02, r"## 6\. Registre MessageType")
    table = find_table(section, ("Valeur", "Nom filaire", "Direction principale", "Payload"))
    parsed: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        message_id = parse_exact_int(cell(row, "Valeur"))
        parsed.append(
            {
                "id": message_id,
                "name": strip_inline_markdown(cell(row, "Nom filaire")),
                "symbol": upper_snake(cell(row, "Nom filaire")),
                "direction": strip_inline_markdown(cell(row, "Direction principale")),
                "payload": strip_inline_markdown(cell(row, "Payload")),
            }
        )

    require_unique(parsed, "id", "MessageType")
    require_unique(parsed, "name", "MessageType")
    require_unique_normalized_names(parsed, "name", "MessageType")
    invalid = next((item for item in parsed if item["id"] == 0), None)
    if invalid is None or normalize_symbol(str(invalid["name"])) != "invalid":
        raise SchemaError("MessageType 0 must be Invalid")
    valid = [item for item in parsed if item["id"] != 0]
    require_contiguous(valid, "id", 1, 20, "MessageType")

    specialized_section = extract_section(doc05, r"### 2\.1 Types de messages spécialisés")
    specialized_table = find_table(specialized_section, ("Valeur", "MessageType", "Direction", "Livraison"))
    by_id = {int(item["id"]): item for item in valid}
    delivery_section = extract_section(doc03, r"### 7\.1 .*Matrice")
    delivery_table = find_table(delivery_section, ("Message", "Livraison"))
    delivery_rows = [row_dict(delivery_table, raw_row) for raw_row in delivery_table.rows]
    specialized_ids: list[int] = []
    for raw_row in specialized_table.rows:
        row = row_dict(specialized_table, raw_row)
        message_id = parse_exact_int(cell(row, "Valeur"))
        specialized_ids.append(message_id)
        specialized_name = strip_inline_markdown(cell(row, "MessageType"))
        if message_id not in by_id:
            raise SchemaError(f"specialized MessageType {message_id} missing from the central registry")
        if normalize_symbol(specialized_name) != normalize_symbol(str(by_id[message_id]["name"])):
            raise SchemaError(
                f"MessageType {message_id} name drift: {specialized_name!r} vs {by_id[message_id]['name']!r}"
            )
        specialized_direction = strip_inline_markdown(cell(row, "Direction"))
        if canonical_direction(specialized_direction) != canonical_direction(str(by_id[message_id]["direction"])):
            raise SchemaError(
                f"MessageType {message_id} direction drift: {specialized_direction!r} "
                f"vs {by_id[message_id]['direction']!r}"
            )
        by_id[message_id]["specialized"] = {
            "direction": specialized_direction,
            "delivery": strip_inline_markdown(cell(row, "Livraison")),
            "source": registry_source("05-capabilities-et-vues-specialisees.md", "2.1"),
        }
        delivery_matches = [
            delivery_row
            for delivery_row in delivery_rows
            if normalize_symbol(specialized_name) in normalize_symbol(cell(delivery_row, "Message"))
        ]
        if not delivery_matches:
            raise SchemaError(f"MessageType {message_id} has no delivery-class row in document 03")
        matrix_tags: set[str] = set()
        for delivery_row in delivery_matches:
            matrix_tags.update(
                canonical_delivery_tags(
                    f"{cell(delivery_row, 'Message')} {cell(delivery_row, 'Livraison')}"
                )
            )
        specialized_tags = canonical_delivery_tags(
            f"{specialized_name} {cell(row, 'Livraison')}"
        )
        if not matrix_tags.issubset(specialized_tags):
            raise SchemaError(
                f"MessageType {message_id} delivery drift: document 03 tags {sorted(matrix_tags)} "
                f"vs document 05 tags {sorted(specialized_tags)}"
            )
        by_id[message_id]["specialized"]["delivery_tags"] = sorted(specialized_tags)
    if specialized_ids != list(range(14, 21)):
        raise SchemaError(f"specialized MessageType IDs must be 14..20, got {specialized_ids}")
    return invalid, valid


MESSAGE_LAYOUT_SPECS: dict[int, tuple[str, str, str, str]] = {
    1: ("02", r"### 9\.2 DiscoveryPayload", "DiscoveryPayload", "9.2"),
    2: ("02", r"### 9\.3 HelloPayload", "HelloPayload", "9.3"),
    3: ("02", r"### 9\.4 WelcomePayload", "WelcomePayload", "9.4"),
    4: ("02", r"### 9\.5 SessionBeginPayload", "SessionBeginPayload", "9.5"),
    5: ("02", r"### 10\.2 ManifestPartPayload", "ManifestPartPayload", "10.2"),
    6: ("02", r"### 10\.3 FullSnapshotPartPayload", "FullSnapshotPartPayload", "10.3"),
    7: ("02", r"### 10\.4 DeltaPayload", "DeltaPayload", "10.4"),
    8: ("02", r"### 10\.5 EventBatchPayload", "EventBatchPayload", "10.5"),
    9: ("02", r"### 9\.6 HeartbeatPayload", "HeartbeatPayload", "9.6"),
    10: ("02", r"### 9\.7 AckPayload", "AckPayload", "9.7"),
    11: ("02", r"### 9\.8 NackPayload", "NackPayload", "9.8"),
    12: ("02", r"### 9\.9 ResyncRequestPayload", "ResyncRequestPayload", "9.9"),
    13: ("02", r"### 9\.10 SessionEndPayload", "SessionEndPayload", "9.10"),
    14: ("05", r"### 7\.1 TARGET_VIDEO_SUBSCRIBE .*", "TARGET_VIDEO_SUBSCRIBE", "7.1"),
    15: ("05", r"### 7\.2 TARGET_VIDEO_CONFIG .*", "TARGET_VIDEO_CONFIG", "7.2"),
    16: ("05", r"### 7\.3 TARGET_VIDEO_FRAME .*", "TARGET_VIDEO_FRAME", "7.3"),
    17: ("05", r"### 7\.4 TARGET_VIDEO_KEYFRAME_REQUEST .*", "TARGET_VIDEO_KEYFRAME_REQUEST", "7.4"),
    18: ("05", r"### 7\.5 TARGET_VIDEO_STOP .*", "TARGET_VIDEO_STOP", "7.5"),
    19: ("05", r"### 7\.6 TARGET_VIDEO_STATS .*", "TARGET_VIDEO_STATS", "7.6"),
    20: ("02", r"### 9\.11 CapabilityUpdatePayload", "CapabilityUpdatePayload", "9.11"),
}


def fixed_wire_size(wire: str) -> int | None:
    scalar_sizes = {
        "u8": 1,
        "i8": 1,
        "bool8": 1,
        "bytes": 1,
        "u16": 2,
        "i16": 2,
        "u32": 4,
        "i32": 4,
        "float32": 4,
        "rgba8": 4,
        "u64": 8,
        "i64": 8,
        "entity_id": 8,
        "asset_id": 8,
        "duration_us": 8,
        "sample_time_us": 8,
        "vec3f": 12,
        "quatf": 16,
        "mat3f": 36,
    }
    if wire in scalar_sizes:
        return scalar_sizes[wire]
    match = re.fullmatch(r"(bytes|u8|i8|u16|i16|u32|i32|u64|i64|float32)\[([0-9]+)\]", wire)
    if not match:
        return None
    item_size = scalar_sizes.get(match.group(1))
    if item_size is None:
        return None
    return item_size * int(match.group(2))


def parse_wire_cell(value: str) -> str:
    code_tokens = re.findall(r"`([^`]+)`", value)
    if code_tokens:
        return code_tokens[0].strip()
    return strip_inline_markdown(value)


def parse_message_qos(doc03: str, messages: Sequence[dict[str, object]]) -> dict[int, list[dict[str, object]]]:
    section = extract_section(doc03, r"### 7\.1 .*Matrice")
    table = find_table(section, ("Message", "Livraison", "ACK requis"))
    rows = [row_dict(table, raw_row) for raw_row in table.rows]
    result: dict[int, list[dict[str, object]]] = {}
    for message in messages:
        message_name = normalize_symbol(str(message["name"]))
        matches: list[dict[str, object]] = []
        for row in rows:
            raw_variant = cell(row, "Message")
            code_tokens = re.findall(r"`([^`]+)`", raw_variant)
            if not code_tokens:
                code_tokens = [strip_inline_markdown(raw_variant)]
            if not any(
                message_name == normalize_symbol(token)
                or normalize_symbol(token).startswith(message_name)
                for token in code_tokens
            ):
                continue
            delivery = strip_inline_markdown(cell(row, "Livraison"))
            ack = strip_inline_markdown(cell(row, "ACK requis"))
            matches.append(
                {
                    "variant": strip_inline_markdown(raw_variant),
                    "delivery": delivery,
                    "ack_required": ack,
                    "delivery_tags": sorted(canonical_delivery_tags(f"{raw_variant} {delivery}")),
                    "source": registry_source("03-session-horloges-fiabilite.md", "7.1"),
                }
            )
        if not matches:
            raise SchemaError(f"MessageType {message['id']} has no machine-readable QoS row")
        result[int(message["id"])] = matches
    return result


def parse_local_payload_limit(section: str) -> int | None:
    patterns = (
        r"payload complet mesure au plus\s+([0-9\s\u00a0\u202f]+)\s+octets",
        r"payload total est limit[^.]*?\s+([0-9\s\u00a0\u202f]+)\s+octets",
        r"taille logique maximale est\s+([0-9\s\u00a0\u202f]+)\s+octets",
        r"encoded_frame_size est donc au plus\s+([0-9\s\u00a0\u202f]+)",
    )
    for pattern in patterns:
        match = re.search(pattern, section, re.IGNORECASE)
        if match:
            return int(re.sub(r"[\s\u00a0\u202f]", "", match.group(1)))
    return None


def parse_message_layouts(
    doc02: str,
    doc03: str,
    doc05: str,
    messages: Sequence[dict[str, object]],
) -> dict[int, dict[str, object]]:
    qos = parse_message_qos(doc03, messages)
    layouts: dict[int, dict[str, object]] = {}
    for message in messages:
        message_id = int(message["id"])
        document_key, heading_pattern, layout_name, section_number = MESSAGE_LAYOUT_SPECS[message_id]
        document = doc02 if document_key == "02" else doc05
        section = extract_section(document, heading_pattern)
        table = find_table(section, ("Offset", "Champ", "Type"))
        fields: list[dict[str, object]] = []
        for raw_row in table.rows:
            row = row_dict(table, raw_row)
            fields.append(
                {
                    "offset": parse_exact_int(cell(row, "Offset")),
                    "name": strip_inline_markdown(cell(row, "Champ")),
                    "wire": parse_wire_cell(cell(row, "Type")),
                    "rule": strip_inline_markdown(optional_cell(row, "Règle")),
                }
            )
        if not fields:
            raise SchemaError(f"MessageType {message_id} has an empty payload layout")
        require_unique(fields, "name", f"MessageType {message_id} fields")
        require_unique_normalized_names(fields, "name", f"MessageType {message_id} fields")
        offsets = [int(field["offset"]) for field in fields]
        if offsets != sorted(offsets) or len(offsets) != len(set(offsets)):
            raise SchemaError(f"MessageType {message_id} offsets collide or regress: {offsets}")

        variable_fields = [
            field
            for field in fields
            if fixed_wire_size(str(field["wire"])) is None
            and int(field["offset"]) == offsets[-1]
        ]
        exact_size: int | None = None
        if not variable_fields:
            final_size = fixed_wire_size(str(fields[-1]["wire"]))
            if final_size is None:
                raise SchemaError(
                    f"MessageType {message_id} final field {fields[-1]['name']} has unknown fixed width"
                )
            exact_size = int(fields[-1]["offset"]) + final_size
        fixed_prefix = int(variable_fields[0]["offset"]) if variable_fields else exact_size
        if fixed_prefix is None:
            raise SchemaError(f"MessageType {message_id} fixed prefix could not be derived")

        local_limit = parse_local_payload_limit(section)
        limits: dict[str, object] = {
            "size_kind": "variable" if variable_fields else "fixed",
            "fixed_prefix_bytes": fixed_prefix,
            "reassembly_limit_bytes": 2_097_152 if message_id == 16 else 1_048_576,
            "reassembly_class": "target-video-frame" if message_id == 16 else "control-state",
        }
        if exact_size is not None:
            limits["exact_size_bytes"] = exact_size
            limits["application_limit_bytes"] = exact_size
        elif local_limit is not None:
            # For TARGET_VIDEO_FRAME, the prose gives both the total limit and the
            # encoded-frame bound. The total reassembly limit is the payload bound.
            limits["application_limit_bytes"] = (
                2_097_152 if message_id == 16 else local_limit
            )
        if message_id == 20:
            limits["v1_exact_size_bytes"] = fixed_prefix

        source_name = (
            "02-format-filaire-et-registres.md"
            if document_key == "02"
            else "05-capabilities-et-vues-specialisees.md"
        )
        layouts[message_id] = {
            "layout_name": layout_name,
            "position_kind": "offset",
            "fields": fields,
            "qos": qos[message_id],
            "limits": limits,
            "source": registry_source(source_name, section_number),
        }
    require_contiguous([{"id": value} for value in layouts], "id", 1, 20, "MessageType layouts")
    return layouts


def parse_record_registry(doc02: str, doc04: str, doc05: str) -> tuple[dict[str, object], list[dict[str, object]]]:
    section02 = extract_section(doc02, r"### 7\.1 RecordType")
    table02 = find_table(section02, ("Valeur", "Nom"))
    central: list[dict[str, object]] = []
    for raw_row in table02.rows:
        row = row_dict(table02, raw_row)
        record_id = parse_exact_int(cell(row, "Valeur"))
        central.append({"id": record_id, "name": strip_inline_markdown(cell(row, "Nom"))})
    require_unique(central, "id", "RecordType")
    require_unique(central, "name", "RecordType")
    require_unique_normalized_names(central, "name", "RecordType")

    invalid = next((item for item in central if item["id"] == 0), None)
    if invalid is None or normalize_symbol(str(invalid["name"])) != "invalid":
        raise SchemaError("RecordType 0 must be Invalid")
    valid_central = [item for item in central if item["id"] != 0]
    require_contiguous(valid_central, "id", 1, 28, "RecordType")

    section04 = extract_section(doc04, r"## 3\. Registre des `RecordType`")
    table04 = find_table(section04, ("Valeur", "Nom", "Scope", "Atome", "CREATE/DELETE"))
    registry04: list[dict[str, object]] = []
    for raw_row in table04.rows:
        row = row_dict(table04, raw_row)
        registry04.append(
            {
                "id": parse_exact_int(cell(row, "Valeur")),
                "name": strip_inline_markdown(cell(row, "Nom")),
                "version": 1,
                "scope": strip_inline_markdown(cell(row, "Scope")),
                "delta_atom": strip_inline_markdown(cell(row, "Atome")),
                "create_delete": strip_inline_markdown(cell(row, "CREATE/DELETE")),
            }
        )
    require_contiguous(registry04, "id", 1, 28, "RecordType model")
    require_unique(registry04, "name", "RecordType model")
    require_unique_normalized_names(registry04, "name", "RecordType model")

    central_by_id = {int(item["id"]): item for item in valid_central}
    for item in registry04:
        central_item = central_by_id[int(item["id"])]
        if normalize_symbol(str(item["name"])) != normalize_symbol(str(central_item["name"])):
            raise SchemaError(
                f"RecordType {item['id']} name drift: {item['name']!r} vs {central_item['name']!r}"
            )

    specialized_section = extract_section(doc05, r"### 2\.2 Types de records de communication")
    specialized_table = find_table(specialized_section, ("Valeur", "RecordType", "Version", "Conteneurs"))
    expected_specialized = {25, 26, 27}
    observed_specialized: set[int] = set()
    registry_by_id = {int(item["id"]): item for item in registry04}
    container_section = extract_section(doc04, r"### 3\.1 Matrice de conteneurs")
    container_table = find_table(container_section, ("Conteneur", "RecordType"))
    containers_by_record: dict[int, set[str]] = {25: set(), 26: set(), 27: set()}
    for raw_row in container_table.rows:
        container_row = row_dict(container_table, raw_row)
        container_names = container_symbols(cell(container_row, "Conteneur"))
        allowed = strip_inline_markdown(cell(container_row, "RecordType"))
        for record_id in containers_by_record:
            if re.search(rf"\b{record_id}\b", allowed):
                containers_by_record[record_id].update(container_names)

    for raw_row in specialized_table.rows:
        row = row_dict(specialized_table, raw_row)
        record_id = parse_exact_int(cell(row, "Valeur"))
        observed_specialized.add(record_id)
        name = strip_inline_markdown(cell(row, "RecordType"))
        version = parse_exact_int(cell(row, "Version"))
        if record_id not in registry_by_id:
            raise SchemaError(f"specialized RecordType {record_id} missing from the model registry")
        if normalize_symbol(name) != normalize_symbol(str(registry_by_id[record_id]["name"])):
            raise SchemaError(f"RecordType {record_id} name drift in specialized registry")
        if version != 1:
            raise SchemaError(f"RecordType {record_id} must use version 1, got {version}")
        specialized_containers = container_symbols(cell(row, "Conteneurs"))
        model_containers = [
            symbol
            for symbol in ("MANIFEST", "FULL_SNAPSHOT", "DELTA", "EVENT_BATCH")
            if symbol in containers_by_record[record_id]
        ]
        if specialized_containers != model_containers:
            raise SchemaError(
                f"RecordType {record_id} container drift: {specialized_containers} vs {model_containers}"
            )
        registry_by_id[record_id]["containers"] = specialized_containers
        registry_by_id[record_id]["specialized_source"] = registry_source(
            "05-capabilities-et-vues-specialisees.md", "2.2"
        )
    if observed_specialized != expected_specialized:
        raise SchemaError(f"specialized RecordType IDs must be 25, 26, 27, got {observed_specialized}")
    return invalid, registry04


def parse_record_layouts(doc04: str, registry: Sequence[dict[str, object]]) -> dict[int, dict[str, object]]:
    heading_re = re.compile(r"^###\s+\d+\.\d+\s+`([^`]+)`\s+—\s+type\s+(\d+)\s*$", re.MULTILINE)
    matches = list(heading_re.finditer(doc04))
    layouts: dict[int, dict[str, object]] = {}
    for index, match in enumerate(matches):
        name = match.group(1)
        record_id = int(match.group(2))
        end = matches[index + 1].start() if index + 1 < len(matches) else len(doc04)
        section = doc04[match.start():end]
        layout_table: MarkdownTable | None = None
        for candidate in parse_tables(section):
            keys = table_header_keys(candidate)
            if any("champ" == key for key in keys) and any("wire" == key for key in keys):
                layout_table = candidate
                break

        fields: list[dict[str, object]] = []
        position_kind = "order"
        if layout_table is not None:
            keys = table_header_keys(layout_table)
            if any(key == "offset" for key in keys):
                position_kind = "offset"
            elif not any(key == "ordre" for key in keys):
                raise SchemaError(f"record {record_id} layout has neither Ordre nor Offset")

            for raw_row in layout_table.rows:
                row = row_dict(layout_table, raw_row)
                position = strip_inline_markdown(cell(row, "Offset" if position_kind == "offset" else "Ordre"))
                field = {
                    "position": position,
                    "name": strip_inline_markdown(cell(row, "Champ")),
                    "wire": strip_inline_markdown(cell(row, "Wire")),
                    "constraint": strip_inline_markdown(cell(row, "Cardinalité/borne", "Borne")),
                    "nature": strip_inline_markdown(cell(row, "Nature")),
                    "semantics": strip_inline_markdown(cell(row, "Présence et sémantique", "Règle")),
                }
                fields.append(field)
        elif record_id == 28:
            payload_match = re.search(r"Le payload est `([A-Za-z_][A-Za-z0-9_]*):([^`]+)`", section)
            if not payload_match:
                raise SchemaError("EVENTS top-level payload declaration not found")
            fields.append(
                {
                    "position": "1",
                    "name": payload_match.group(1),
                    "wire": payload_match.group(2),
                    "constraint": "",
                    "nature": "E",
                    "semantics": "top-level payload declared normatively in prose",
                }
            )
        else:
            raise SchemaError(f"record {record_id} {name} has no top-level field/layout table")

        if record_id in layouts:
            raise SchemaError(f"duplicate top-level layout for RecordType {record_id}")
        if not fields:
            raise SchemaError(f"empty top-level layout for RecordType {record_id}")
        require_unique(fields, "name", f"RecordType {record_id} fields")
        require_unique_normalized_names(fields, "name", f"RecordType {record_id} fields")

        if position_kind == "order":
            observed_positions = [parse_exact_int(str(field["position"])) for field in fields]
            if observed_positions != list(range(1, len(fields) + 1)):
                raise SchemaError(f"RecordType {record_id} field order is not contiguous: {observed_positions}")
        else:
            numeric_offsets = [
                parse_exact_int(str(field["position"]))
                for field in fields
                if re.fullmatch(r"[0-9]+", str(field["position"]))
            ]
            if numeric_offsets != sorted(numeric_offsets) or len(numeric_offsets) != len(set(numeric_offsets)):
                raise SchemaError(f"RecordType {record_id} offsets collide or regress: {numeric_offsets}")

        layouts[record_id] = {
            "layout_name": name,
            "position_kind": position_kind,
            "fields": fields,
        }

    require_contiguous([{"id": value} for value in layouts], "id", 1, 28, "RecordType layouts")
    by_id = {int(item["id"]): item for item in registry}
    for record_id, layout in layouts.items():
        if normalize_symbol(str(layout["layout_name"])) != normalize_symbol(str(by_id[record_id]["name"])):
            raise SchemaError(f"RecordType {record_id} layout heading does not match registry")
    return layouts


def extract_marked_region(text: str, start_marker: str, end_marker: str | None) -> tuple[str, int]:
    start = text.find(start_marker)
    if start < 0:
        raise SchemaError(f"nested-layout marker not found: {start_marker!r}")
    if end_marker is None:
        end = text.find("\n\n", start)
    else:
        end = text.find(end_marker, start + len(start_marker))
    if end < 0:
        raise SchemaError(f"nested-layout end marker not found after {start_marker!r}: {end_marker!r}")
    return text[start:end], start


def section_number_at(text: str, offset: int) -> str:
    result = "2.1"
    for match in re.finditer(r"^###\s+([0-9]+\.[0-9]+)\s+", text[:offset], re.MULTILINE):
        result = match.group(1)
    return result


def parse_inline_structure_fields(
    region: str,
    untyped_fields: dict[str, str] | None = None,
    layout_start_marker: str | None = None,
) -> list[dict[str, object]]:
    layout = region
    if layout_start_marker is not None:
        start = region.find(layout_start_marker)
        if start < 0:
            raise SchemaError(f"nested layout marker missing from region: {layout_start_marker!r}")
        layout = region[start + len(layout_start_marker):]
    overrides = untyped_fields or {}
    fields: list[dict[str, object]] = []
    declaration_re = re.compile(
        r"^(?P<name>[a-z][a-z0-9_]*):(?P<wire>[A-Za-z][A-Za-z0-9_]*(?:<[^`\s]+>|\[[^`\s]+\])?)"
    )
    for match in re.finditer(r"`([^`\n]+)`", layout):
        declaration = match.group(1).strip()
        typed = declaration_re.match(declaration)
        if typed:
            field_name = typed.group("name")
            wire = typed.group("wire")
        elif declaration in overrides:
            field_name = declaration
            wire = overrides[declaration]
        else:
            continue
        line_start = layout.rfind("\n", 0, match.start()) + 1
        line_end = layout.find("\n", match.end())
        if line_end < 0:
            line_end = len(layout)
        fields.append(
            {
                "position": len(fields) + 1,
                "name": field_name,
                "wire": wire,
                "declaration": declaration,
                "rule": strip_inline_markdown(layout[line_start:line_end].strip()),
            }
        )
    if not fields:
        raise SchemaError("nested layout has no typed field declarations")
    occurrences: dict[str, int] = {}
    for field in fields:
        name = str(field["name"])
        occurrences[name] = occurrences.get(name, 0) + 1
        field["occurrence"] = occurrences[name]
    return fields


def parse_presence_bits(region: str) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for name, bit in re.findall(r"`?([A-Z][A-Z0-9_]+)=bit\s*([0-9]+)`?", region):
        result.append({"name": name, "bit": int(bit), "mask": 1 << int(bit)})
    deduplicated: dict[tuple[str, int], dict[str, object]] = {}
    for item in result:
        deduplicated[(str(item["name"]), int(item["bit"]))] = item
    return list(deduplicated.values())


def structured_type_references(wire: str) -> list[str]:
    return re.findall(r"\b([A-Z][A-Za-z0-9]+V1)\b", wire)


def parse_wire_aliases(doc04: str) -> list[dict[str, object]]:
    section = extract_section(doc04, r"### 2\.1 Types .*g.*s")
    table = find_table(section, ("Notation", "Encodage"))
    aliases: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        notation_cell = cell(row, "Notation")
        notation = strip_inline_markdown(notation_cell)
        rule = strip_inline_markdown(cell(row, "Encodage"))
        code_tokens = re.findall(r"`([^`]+)`", cell(row, "Encodage"))
        if not code_tokens:
            raise SchemaError(f"wire alias {notation!r} has no encoded representation")
        representation = code_tokens[0].split(",", 1)[0].strip()
        notation_tokens = re.findall(r"`([^`]+)`", notation_cell)
        if not notation_tokens:
            notation_tokens = [notation]
        for name in notation_tokens:
            aliases.append(
                {
                    "name": name,
                    "wire": representation,
                    "rule": rule,
                    "source": registry_source("04-modele-de-donnees-v1.md", "2.1"),
                }
            )
    require_unique(aliases, "name", "wire aliases")
    return aliases


def parse_asset_entry_structure(doc04: str) -> dict[str, object]:
    region, offset = extract_marked_region(doc04, "Chaque `AssetEntryV1`", "`FORMAT_INTRINSIC`")
    table = find_table(region, ("Offset relatif", "Champ", "Wire"))
    fields: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        fields.append(
            {
                "position": strip_inline_markdown(cell(row, "Offset relatif")),
                "name": strip_inline_markdown(cell(row, "Champ")),
                "wire": strip_inline_markdown(cell(row, "Wire")),
                "constraint": strip_inline_markdown(cell(row, "Cardinalité/borne")),
                "nature": strip_inline_markdown(cell(row, "Nature")),
                "rule": strip_inline_markdown(cell(row, "Sémantique")),
                "occurrence": 1,
            }
        )
    require_unique(fields, "name", "AssetEntryV1 fields")
    return {
        "name": "AssetEntryV1",
        "version": 1,
        "position_kind": "offset",
        "fixed_prefix_bytes": 72,
        "fields": fields,
        "source": registry_source("04-modele-de-donnees-v1.md", section_number_at(doc04, offset)),
    }


def parse_record_structured_types(doc04: str) -> list[dict[str, object]]:
    # The document deliberately describes most nested records inline. Each
    # region below is bounded by its adjacent normative declaration; fields
    # are extracted from the inline ``name:wire`` tokens in source order.
    specs: dict[str, tuple[str, str | None, str | None]] = {
        "ClassMotionV1": ("- `ClassMotionV1`", "- `ClassEnergyV1`", None),
        "ClassEnergyV1": ("- `ClassEnergyV1`", "- `ClassAfterburnerV1`", None),
        "ClassAfterburnerV1": ("- `ClassAfterburnerV1`", "- `ClassCountermeasureV1`", None),
        "ClassCountermeasureV1": ("- `ClassCountermeasureV1`", "- `ClassBankV1`", None),
        "ClassBankV1": ("- `ClassBankV1`", "- `ClassSubsystemV1`", None),
        "ClassSubsystemV1": ("- `ClassSubsystemV1`", "- `ClassScanV1`", None),
        "ClassScanV1": ("- `ClassScanV1`", "\n\nLes champs de table", None),
        "FlightTimeConstantsV1": ("`FlightTimeConstantsV1` contient", None, None),
        "DamageContributorV1": ("`DamageContributorV1` :", None, None),
        "SubsystemAnimationV1": ("`SubsystemAnimationV1` contient", "`TurretStateV1` utilise", None),
        "TurretStateV1": ("`TurretStateV1` utilise", "`TurretBankV1` :", "Les champs suivent cet ordre :"),
        "TurretBankV1": ("`TurretBankV1` :", None, None),
        "PrimaryBankV1": ("`PrimaryBankV1` utilise", "`SecondaryBankV1` utilise", "Ordre exact :"),
        "SecondaryBankV1": ("`SecondaryBankV1` utilise", "`TertiaryBankV1` contient", "Ordre exact :"),
        "TertiaryBankV1": ("`TertiaryBankV1` contient", "`CountermeasureStateV1` :", None),
        "CountermeasureStateV1": ("`CountermeasureStateV1` :", None, None),
        "LockItemV1": ("`LockItemV1` :", None, None),
        "IncomingMissileV1": ("`IncomingMissileV1` utilise", None, None),
        "DockingRelationV1": ("`DockingRelationV1` :", None, None),
        "NavPointV1": ("`NavPointV1` :", "`WaypointV1` :", None),
        "WaypointV1": ("`WaypointV1` :", None, None),
        "TagEffectV1": ("`TagEffectV1` :", "`ScalarVisualV1` :", None),
        "ScalarVisualV1": ("`ScalarVisualV1` :", None, None),
        "EventItemV1": ("Le payload est `events:vlist<EventItemV1,256>`", "Règles de présence par kind", "Ordre de `EventItemV1` :"),
    }
    untyped_fields: dict[str, dict[str, str]] = {
        "ClassMotionV1": {
            "forward_accel_time_const": "float32",
            "afterburner_forward_accel_time_const": "float32",
            "booster_forward_accel_time_const": "float32",
            "forward_decel_time_const": "float32",
            "slide_accel_time_const": "float32",
            "slide_decel_time_const": "float32",
        },
        "ClassEnergyV1": {
            "power_output": "float32",
            "reserve_energy_max": "float32",
            "weapon_energy_max": "float32",
            "weapon_regen_per_s": "float32",
            "shield_regen_per_s": "float32",
        },
        "ClassAfterburnerV1": {
            "fuel_max": "float32",
            "burn_per_s": "float32",
            "recover_per_s": "float32",
            "minimum_to_engage": "float32",
        },
        "FlightTimeConstantsV1": {
            "forward_accel": "float32",
            "afterburner_forward_accel": "float32",
            "booster_forward_accel": "float32",
            "forward_decel": "float32",
            "slide_accel": "float32",
            "slide_decel": "float32",
        },
    }
    structures: list[dict[str, object]] = []
    for name, (start_marker, end_marker, field_start_marker) in specs.items():
        region, offset = extract_marked_region(doc04, start_marker, end_marker)
        fields = parse_inline_structure_fields(
            region,
            untyped_fields=untyped_fields.get(name),
            layout_start_marker=field_start_marker,
        )
        item: dict[str, object] = {
            "name": name,
            "version": 1,
            "position_kind": "order",
            "fields": fields,
            "source": registry_source("04-modele-de-donnees-v1.md", section_number_at(doc04, offset)),
        }
        presence_bits = parse_presence_bits(region)
        if presence_bits:
            item["presence_bits"] = presence_bits
        structures.append(item)
    structures.append(parse_asset_entry_structure(doc04))

    require_unique(structures, "name", "nested record structures")
    documented = set(re.findall(r"\b([A-Z][A-Za-z0-9]+V1)\b", doc04))
    parsed = {str(item["name"]) for item in structures}
    if parsed != documented:
        raise SchemaError(
            f"nested record structure coverage drift; missing={sorted(documented - parsed)}, "
            f"unexpected={sorted(parsed - documented)}"
        )
    for structure in structures:
        structure["nested_types"] = sorted(
            {
                reference
                for field in structure["fields"]
                for reference in structured_type_references(str(field["wire"]))
            }
        )
    return sorted(structures, key=lambda item: str(item["name"]))


def parse_capabilities(doc02: str, doc05: str) -> list[dict[str, object]]:
    section02 = extract_section(doc02, r"### 9\.1 Capabilities communes")
    table02 = find_table(section02, ("Bit", "Valeur", "Nom", "Rôle"))
    central: dict[int, dict[str, object]] = {}
    for raw_row in table02.rows:
        row = row_dict(table02, raw_row)
        try:
            bit = parse_exact_int(cell(row, "Bit"))
        except SchemaError:
            continue
        central[bit] = {
            "bit": bit,
            "mask": parse_exact_int(cell(row, "Valeur")),
            "name": strip_inline_markdown(cell(row, "Nom")),
            "role": strip_inline_markdown(cell(row, "Rôle")),
        }

    section05 = extract_section(doc05, r"### 3\.1 Bitmap Capability")
    table05 = find_table(section05, ("Bit", "Masque", "Nom", "Propriétaire"))
    specialized: dict[int, dict[str, object]] = {}
    for raw_row in table05.rows:
        row = row_dict(table05, raw_row)
        try:
            bit = parse_exact_int(cell(row, "Bit"))
        except SchemaError:
            continue
        specialized[bit] = {
            "bit": bit,
            "mask": parse_exact_int(cell(row, "Masque")),
            "name": strip_inline_markdown(cell(row, "Nom")),
            "owner": strip_inline_markdown(cell(row, "Propriétaire")),
        }

    if sorted(central) != list(range(5)) or sorted(specialized) != list(range(5)):
        raise SchemaError("Capability bits must be exactly 0..4 in both normative registries")
    result: list[dict[str, object]] = []
    for bit in range(5):
        left = central[bit]
        right = specialized[bit]
        if left["mask"] != right["mask"] or normalize_symbol(str(left["name"])) != normalize_symbol(str(right["name"])):
            raise SchemaError(f"Capability bit {bit} drifts between documents 02 and 05")
        result.append({**left, "owner": right["owner"]})
    require_unique(result, "mask", "Capability")
    require_unique(result, "name", "Capability")
    require_unique_normalized_names(result, "name", "Capability")
    for item in result:
        if int(item["mask"]) != 1 << int(item["bit"]):
            raise SchemaError(f"Capability {item['name']} mask does not equal 1 << bit")
    return result


def parse_validation_errors(doc06: str) -> list[dict[str, object]]:
    section = extract_section(doc06, r"## 7\. Taxonomie locale des validations")
    table = find_table(section, ("Valeur", "Code", "Niveau"))
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        result.append(
            {
                "id": parse_exact_int(cell(row, "Valeur")),
                "name": strip_inline_markdown(cell(row, "Code")),
                "level": strip_inline_markdown(cell(row, "Niveau")),
            }
        )
    require_contiguous(result, "id", 0, 47, "ValidationError")
    require_unique(result, "name", "ValidationError")
    require_unique_normalized_names(result, "name", "ValidationError")
    return result


def parse_doc02_numeric_registries(
    doc02: str, capabilities: Sequence[dict[str, object]]
) -> dict[str, dict[str, object]]:
    registries: dict[str, dict[str, object]] = {}

    message_flags_section = extract_section(doc02, r"### 4\.3 .*MessageFlags")
    message_flags = values_from_bit_table(
        find_table(message_flags_section, ("Bit", "Valeur", "Nom"))
    )
    registries["MessageFlags"] = make_bitmask_registry(
        "MessageFlags",
        8,
        message_flags,
        registry_source("02-format-filaire-et-registres.md", "4.3"),
        notes=("KEYFRAME and VIDEO_IDR are mutually exclusive",),
    )

    record_flags_section = extract_section(doc02, r"### 7\.3 .*RecordFlags.*")
    record_flags = values_from_bit_table(find_table(record_flags_section, ("Bit", "Valeur", "Nom")))
    partial = next((item for item in record_flags if item["name"] == "PARTIAL"), None)
    if partial is None:
        raise SchemaError("RecordFlags.PARTIAL assignment is missing")
    partial["emittable_v1"] = False
    partial["valid_record_types_v1"] = []
    registries["RecordFlags"] = make_bitmask_registry(
        "RecordFlags",
        8,
        record_flags,
        registry_source("02-format-filaire-et-registres.md", "7.3"),
        notes=("CREATE, DELETE and PARTIAL combination constraints are normative",),
    )

    capability_values = [
        {
            "name": upper_snake(str(item["name"])),
            "value": int(item["mask"]),
            "bit": int(item["bit"]),
            "owner": item["owner"],
            "role": item["role"],
        }
        for item in capabilities
    ]
    registries["Capability"] = make_bitmask_registry(
        "Capability",
        64,
        capability_values,
        registry_source("02-format-filaire-et-registres.md", "9.1"),
        extensible=True,
        notes=("Unknown bits are ignored, excluded from active_capabilities, and never relayed",),
    )

    welcome_section = extract_section(doc02, r"### 9\.4 .*WelcomePayload")
    welcome_values = values_from_value_name_table(
        table_after(welcome_section, r"WelcomeStatus", ("Valeur", "Nom"))
    )
    registries["WelcomeStatus"] = make_enum_registry(
        "WelcomeStatus", 8, welcome_values, registry_source("02-format-filaire-et-registres.md", "9.4")
    )

    session_begin_section = extract_section(doc02, r"### 9\.5 .*SessionBeginPayload")
    session_begin_values = values_from_bit_table(
        table_after(session_begin_section, r"SessionBeginFlags", ("Bit", "Valeur", "Nom"))
    )
    registries["SessionBeginFlags"] = make_bitmask_registry(
        "SessionBeginFlags",
        32,
        session_begin_values,
        registry_source("02-format-filaire-et-registres.md", "9.5"),
        notes=("READ_ONLY is mandatory",),
    )

    heartbeat_section = extract_section(doc02, r"### 9\.6 .*HeartbeatPayload")
    heartbeat_table = find_table(heartbeat_section, ("Offset", "Champ", "Type"))
    heartbeat_values: list[dict[str, object]] = []
    for raw_row in heartbeat_table.rows:
        row = row_dict(heartbeat_table, raw_row)
        if keyify(cell(row, "Champ")) == "kind":
            heartbeat_values = backtick_number_names(raw_row[3])
            break
    registries["HeartbeatKind"] = make_enum_registry(
        "HeartbeatKind",
        8,
        heartbeat_values,
        registry_source("02-format-filaire-et-registres.md", "9.6"),
        reserved_ranges=((0, 0), (3, 255)),
    )

    ack_section = extract_section(doc02, r"### 9\.7 .*AckPayload")
    ack_values = values_from_bit_table(table_after(ack_section, r"AckFlags", ("Bit", "Valeur", "Nom")))
    registries["AckFlags"] = make_bitmask_registry(
        "AckFlags",
        8,
        ack_values,
        registry_source("02-format-filaire-et-registres.md", "9.7"),
        notes=("APPLIED implies VALIDATED; zero is invalid",),
    )

    nack_section = extract_section(doc02, r"### 9\.8 .*NackPayload")
    nack_values = values_from_value_name_table(table_after(nack_section, r"NackReason", ("Valeur", "Nom")))
    registries["NackReason"] = make_enum_registry(
        "NackReason", 8, nack_values, registry_source("02-format-filaire-et-registres.md", "9.8")
    )

    resync_section = extract_section(doc02, r"### 9\.9 .*ResyncRequestPayload")
    resync_values = values_from_value_name_table(
        table_after(resync_section, r"ResyncReason", ("Valeur", "Nom"))
    )
    registries["ResyncReason"] = make_enum_registry(
        "ResyncReason", 8, resync_values, registry_source("02-format-filaire-et-registres.md", "9.9")
    )
    resync_flag_match = re.search(r"`ResyncRequestFlags`\s*:\s*(.+)", resync_section)
    if resync_flag_match is None:
        raise SchemaError("ResyncRequestFlags normative sentence not found")
    registries["ResyncRequestFlags"] = make_bitmask_registry(
        "ResyncRequestFlags",
        8,
        named_bits(resync_flag_match.group(1)),
        registry_source("02-format-filaire-et-registres.md", "9.9"),
        notes=("REQUIRE_FULL_SNAPSHOT is mandatory",),
    )

    session_end_section = extract_section(doc02, r"### 9\.10 .*SessionEndPayload")
    session_end_match = re.search(r"`SessionEndReason`\s*:\s*(.+)", session_end_section)
    if session_end_match is None:
        raise SchemaError("SessionEndReason normative sentence not found")
    session_end_values = backtick_number_names(session_end_match.group(1))
    registries["SessionEndReason"] = make_enum_registry(
        "SessionEndReason",
        8,
        session_end_values,
        registry_source("02-format-filaire-et-registres.md", "9.10"),
        reserved_ranges=((0, 0), (7, 255)),
    )
    if "RECONNECT_ALLOWED" not in session_end_match.group(1):
        raise SchemaError("SessionEndFlag RECONNECT_ALLOWED is missing")
    end_flag_values = [{"name": "RECONNECT_ALLOWED", "bit": 0, "value": 1}]
    registries["SessionEndFlags"] = make_bitmask_registry(
        "SessionEndFlags",
        8,
        end_flag_values,
        registry_source("02-format-filaire-et-registres.md", "9.10"),
    )

    update_section = extract_section(doc02, r"### 9\.11 .*CapabilityUpdatePayload")
    update_match = re.search(r"`CapabilityUpdateReason`\s*:\s*(.+)", update_section)
    if update_match is None:
        raise SchemaError("CapabilityUpdateReason normative sentence not found")
    update_values = [{"name": "INVALID", "value": 0}]
    update_values.extend(backtick_number_names(update_match.group(1).split(";", 1)[0]))
    registries["CapabilityUpdateReason"] = make_enum_registry(
        "CapabilityUpdateReason",
        8,
        update_values,
        registry_source("02-format-filaire-et-registres.md", "9.11"),
        reserved_ranges=((5, 255),),
    )

    manifest_section = extract_section(doc02, r"### 10\.2 .*ManifestPartPayload")
    manifest_match = re.search(r"`ManifestKind`\s+(.+)", manifest_section)
    if manifest_match is None:
        raise SchemaError("ManifestKind normative sentence not found")
    manifest_values = backtick_number_names(manifest_match.group(1).split("en v1.0", 1)[0])
    registries["ManifestKind"] = make_enum_registry(
        "ManifestKind",
        16,
        manifest_values,
        registry_source("02-format-filaire-et-registres.md", "10.2"),
        reserved_ranges=((0, 0), (2, 65535)),
    )

    snapshot_section = extract_section(doc02, r"### 10\.3 .*FullSnapshotPartPayload")
    snapshot_match = re.search(r"`SnapshotFlags`\s*:\s*(.+)", snapshot_section)
    if snapshot_match is None:
        raise SchemaError("SnapshotFlags normative sentence not found")
    registries["SnapshotFlags"] = make_bitmask_registry(
        "SnapshotFlags",
        16,
        named_bits(snapshot_match.group(1)),
        registry_source("02-format-filaire-et-registres.md", "10.3"),
        notes=("Exactly one of INITIAL, PERIODIC_KEYFRAME, or RESYNC is required",),
    )

    event_batch_section = extract_section(doc02, r"### 10\.5 .*EventBatchPayload")
    delivery_values = [
        {"name": upper_snake(name), "value": int(value)}
        for value, name in re.findall(
            r"delivery_class\s*==\s*([0-9]+).*?`([A-Za-z]+)`", event_batch_section
        )
    ]
    registries["EventDeliveryClass"] = make_enum_registry(
        "EventDeliveryClass",
        8,
        delivery_values,
        registry_source("02-format-filaire-et-registres.md", "10.5"),
        reserved_ranges=((0, 0), (3, 255)),
    )

    hello_section = extract_section(doc02, r"### 9\.3 .*HelloPayload")
    visibility_match = re.search(r"`VisibilityMode`.*?`0\s+([A-Z_]+)`.*?`1\s+([A-Z_]+)`", hello_section)
    if visibility_match is None:
        raise SchemaError("VisibilityMode normative values not found")
    registries["VisibilityMode"] = make_enum_registry(
        "VisibilityMode",
        8,
        [
            {"name": visibility_match.group(1), "value": 0},
            {"name": visibility_match.group(2), "value": 1},
        ],
        registry_source("02-format-filaire-et-registres.md", "9.3"),
        reserved_ranges=((2, 255),),
    )
    return registries


def parse_doc04_numeric_registries(doc04: str) -> dict[str, dict[str, object]]:
    registries: dict[str, dict[str, object]] = {}

    enum_section = extract_section(doc04, r"### 4\.1 .*Enums")
    enum_table = find_table(enum_section, ("Enum", "Valeurs"))
    for raw_row in enum_table.rows:
        row = row_dict(enum_table, raw_row)
        raw_name = strip_inline_markdown(cell(row, "Enum"))
        name_match = re.match(r"([A-Za-z][A-Za-z0-9_]*)", raw_name)
        if name_match is None:
            raise SchemaError(f"invalid common enum name {raw_name!r}")
        name = name_match.group(1)
        width_bits = 16 if "u16" in raw_name else 8
        values = numeric_assignments(cell(row, "Valeurs"))
        registries[name] = make_enum_registry(
            name,
            width_bits,
            values,
            registry_source("04-modele-de-donnees-v1.md", "4.1"),
        )

    bitset_section = extract_section(doc04, r"### 4\.2 .*Bitsets.*")
    bitset_table = find_table(bitset_section, ("Bitset", "Bits v1"))
    for raw_row in bitset_table.rows:
        row = row_dict(bitset_table, raw_row)
        raw_name = strip_inline_markdown(cell(row, "Bitset"))
        name_match = re.fullmatch(r"([A-Za-z][A-Za-z0-9_]*):u(8|16|32|64)", raw_name)
        if name_match is None:
            raise SchemaError(f"invalid common bitset name/type {raw_name!r}")
        name = name_match.group(1)
        width_bits = int(name_match.group(2))
        registries[name] = make_bitmask_registry(
            name,
            width_bits,
            numeric_assignments(cell(row, "Bits v1")),
            registry_source("04-modele-de-donnees-v1.md", "4.2"),
        )

    event_section = extract_section(doc04, r"### 9\.4 .*EVENTS.*")
    event_table = find_table(event_section, ("Valeur", "Nom"))
    event_values: list[dict[str, object]] = []
    if len(event_table.headers) != 4:
        raise SchemaError("EventKind table must contain two value/name pairs")
    for raw_row in event_table.rows:
        for value_index, name_index in ((0, 1), (2, 3)):
            raw_value = strip_inline_markdown(raw_row[value_index])
            raw_name = strip_inline_markdown(raw_row[name_index])
            if not raw_value and not raw_name:
                continue
            event_values.append({"name": upper_snake(raw_name), "value": parse_exact_int(raw_value)})
    registries["EventKind"] = make_enum_registry(
        "EventKind",
        16,
        event_values,
        registry_source("04-modele-de-donnees-v1.md", "9.4"),
        reserved_ranges=((0, 0), (34, 65535)),
    )

    inline_specs = (
        ("PropulsionFlags", 16, r"`PropulsionFlags:u16`\s+fixe\s+(.+?);\s+autres bits", "7.3"),
        ("ScanValidityFlags", 8, r"`ScanValidityFlags:u8`\s+fixe\s+(.+?);\s+autres bits", "8.4"),
        ("SupportFlags", 8, r"`SupportFlags:u8`\s+fixe\s+(.+?);\s+les autres bits", "8.6"),
        ("NavPointFlags", 8, r"`NavPointFlags:u8`\s+fixe\s+(.+?);\s+les autres bits", "8.7"),
        ("EventFlags", 16, r"`EventFlags:u16`\s+fixe\s+(.+?);\s+les autres bits", "9.4"),
    )
    for name, width_bits, pattern, section_number in inline_specs:
        match = re.search(pattern, doc04)
        if match is None:
            raise SchemaError(f"{name} normative assignment sentence not found")
        registries[name] = make_bitmask_registry(
            name,
            width_bits,
            numeric_assignments(match.group(1)),
            registry_source("04-modele-de-donnees-v1.md", section_number),
        )

    countermeasure_section = extract_section(doc04, r"### 7\.4 .*WEAPON_STATE.*")
    countermeasure_match = re.search(
        r"`CountermeasureStateV1`\s*:.*?`flags:u16`\s*\(([^)]+)\)", countermeasure_section
    )
    if countermeasure_match is None:
        raise SchemaError("CountermeasureStateFlags normative assignments not found")
    registries["CountermeasureStateFlags"] = make_bitmask_registry(
        "CountermeasureStateFlags",
        16,
        numeric_assignments(countermeasure_match.group(1)),
        registry_source("04-modele-de-donnees-v1.md", "7.4"),
        notes=("AVAILABLE and LOCKED are mutually exclusive",),
    )

    manifest_section = extract_section(doc04, r"### 9\.1 .*COMM_ASSET_MANIFEST.*")
    manifest_line = next((line for line in manifest_section.splitlines() if "`ManifestFlags:u16`" in line), None)
    if manifest_line is None:
        raise SchemaError("ManifestFlags normative assignments not found in document 04")
    manifest_values = numeric_assignments(manifest_line.split(".", 1)[0])
    registries["ManifestFlags"] = make_bitmask_registry(
        "ManifestFlags",
        16,
        manifest_values,
        registry_source("04-modele-de-donnees-v1.md", "9.1"),
    )
    for enum_name in ("AlphaMode", "AssetTimingMode"):
        enum_match = re.search(rf"`{enum_name}:u8`[^.]*?((?:`[A-Z][A-Z0-9_]*=[0-9]+`[^.]*)+)", manifest_line)
        if enum_match is None:
            raise SchemaError(f"{enum_name} assignments not found in document 04")
        registries[enum_name] = make_enum_registry(
            enum_name,
            8,
            numeric_assignments(enum_match.group(1)),
            registry_source("04-modele-de-donnees-v1.md", "9.1"),
        )

    heading_pattern = re.compile(
        r"^###\s+(\d+\.\d+)\s+`([^`]+)`.*?type\s+(\d+)\s*$", re.MULTILINE
    )
    headings = list(heading_pattern.finditer(doc04))
    record_presence_names: list[str] = []
    for index, heading in enumerate(headings):
        record_id = int(heading.group(3))
        if record_id > 24:
            continue
        end = headings[index + 1].start() if index + 1 < len(headings) else len(doc04)
        record_section = doc04[heading.start():end]
        layout_tables = parse_tables(record_section)
        layout_table = next(
            (
                table
                for table in layout_tables
                if "champ" in table_header_keys(table) and "wire" in table_header_keys(table)
            ),
            None,
        )
        if layout_table is None:
            raise SchemaError(f"RecordType {record_id} has no layout while deriving its presence mask")
        presence_row = next(
            (raw_row for raw_row in layout_table.rows if keyify(raw_row[1]) == "presence"),
            None,
        )
        if presence_row is None:
            raise SchemaError(f"RecordType {record_id} has no top-level presence field")
        wire_match = re.fullmatch(r"u(8|16|32|64)", strip_inline_markdown(presence_row[2]))
        if wire_match is None:
            raise SchemaError(f"RecordType {record_id} presence field has invalid wire type")
        width_bits = int(wire_match.group(1))
        prose_prefix = record_section.split("|", 1)[0]
        indexed_match = re.search(r"Presence bits\s*:\s*(.+?);", prose_prefix)
        if indexed_match is not None:
            index_values = numeric_assignments(indexed_match.group(1))
            values = [
                {"name": item["name"], "bit": int(item["value"]), "value": 1 << int(item["value"])}
                for item in index_values
            ]
        else:
            values = named_bits(prose_prefix)
        if not values and "aucun bit" not in prose_prefix:
            raise SchemaError(f"RecordType {record_id} presence mask has no parseable definition")
        registry_name = "".join(part.title() for part in heading.group(2).split("_")) + "Presence"
        registries[registry_name] = make_bitmask_registry(
            registry_name,
            width_bits,
            values,
            registry_source("04-modele-de-donnees-v1.md", heading.group(1)),
            allow_empty=True,
            notes=(f"Top-level presence bitmap for RecordType {record_id}",),
        )
        record_presence_names.append(registry_name)
    if len(record_presence_names) != 24:
        raise SchemaError(f"expected 24 top-level record presence registries, got {record_presence_names}")

    nested_presence_names: list[str] = []
    for line in doc04.splitlines():
        field_match = re.search(r"`(?:item_presence|turret_presence):u(8|16|32|64)`", line)
        if field_match is None or "=bit" not in line:
            continue
        type_names = re.findall(r"([A-Za-z][A-Za-z0-9_]*V1)", line)
        if not type_names:
            raise SchemaError(f"nested presence definition lacks a V1 type name: {line!r}")
        type_name = type_names[0]
        registry_name = type_name.removesuffix("V1") + "Presence"
        if registry_name in registries:
            raise SchemaError(f"duplicate nested presence registry {registry_name}")
        values = named_bits(line)
        registries[registry_name] = make_bitmask_registry(
            registry_name,
            int(field_match.group(1)),
            values,
            registry_source("04-modele-de-donnees-v1.md", f"nested {type_name}"),
            notes=(f"Nested presence bitmap for {type_name}",),
        )
        nested_presence_names.append(registry_name)
    expected_nested = {
        "ClassBankPresence",
        "ClassSubsystemPresence",
        "SubsystemAnimationPresence",
        "TurretStatePresence",
        "TurretBankPresence",
        "PrimaryBankPresence",
        "SecondaryBankPresence",
        "CountermeasureStatePresence",
        "LockItemPresence",
        "IncomingMissilePresence",
        "NavPointPresence",
        "EventItemPresence",
    }
    if set(nested_presence_names) != expected_nested:
        raise SchemaError(
            f"nested presence registry coverage drift; expected={sorted(expected_nested)}, "
            f"got={sorted(nested_presence_names)}"
        )
    all_presence_names = set(record_presence_names) | expected_nested
    presence_bit_count = sum(len(registries[name]["values"]) for name in all_presence_names)
    if presence_bit_count != 196:
        raise SchemaError(f"presence bitmap inventory must contain exactly 196 assigned bits, got {presence_bit_count}")
    return registries


def parse_doc05_numeric_registries(
    doc02: str, doc05: str, common: dict[str, dict[str, object]]
) -> dict[str, dict[str, object]]:
    registries: dict[str, dict[str, object]] = {}

    extension_section = extract_section(doc05, r"### 3\.2 .*HELLO")
    extension_table = find_table(extension_section, ("extension_type", "Nom", "Version"))
    extension_values = [{"name": "INVALID", "value": 0}]
    extension_values.extend(
        values_from_value_name_table(extension_table, value_header="extension_type", name_header="Nom")
    )
    for item in extension_values:
        value = int(item["value"])
        item["emittable_v1"] = value == 1
        if value == 0:
            item["status"] = "invalid"
        elif value == 1:
            item["status"] = "active_v1"
        elif value == 2:
            item["status"] = "reserved_not_emitted_v1"
    extension_prose = extract_section(doc02, r"### 9\.1 .*Capabilities.*")
    if not re.search(r"extension_type\s*==\s*0.*?invalide", strip_inline_markdown(extension_prose)):
        raise SchemaError("CapabilityExtensionType zero-invalid rule is missing")
    for expected_id in (1, 2):
        if not re.search(rf"`{expected_id}`[^.]*?(?:communication|vid\S*o)", extension_prose):
            raise SchemaError(f"CapabilityExtensionType {expected_id} drifts from document 02")
    registries["CapabilityExtensionType"] = make_enum_registry(
        "CapabilityExtensionType",
        16,
        extension_values,
        registry_source("05-capabilities-et-vues-specialisees.md", "3.2"),
        unknown_policy="ignore",
        unknown_handling="skip the extension envelope by extension_length",
        reserved_ranges=((3, 65535),),
        notes=("Unknown extension envelopes are skipped by extension_length",),
    )

    negotiation_section = extract_section(doc05, r"### 3\.3 .*WELCOME")
    negotiation_values = values_from_value_name_table(
        table_after(negotiation_section, r"CommNegotiationResult", ("Valeur", "Nom"))
    )
    registries["CommNegotiationResult"] = make_enum_registry(
        "CommNegotiationResult",
        8,
        negotiation_values,
        registry_source("05-capabilities-et-vues-specialisees.md", "3.3"),
    )

    update_section = extract_section(doc05, r"### 3\.4 .*CAPABILITY_UPDATE")
    update_values = values_from_value_name_table(
        table_after(update_section, r"CapabilityUpdateReason", ("Valeur", "Nom"))
    )
    assert_same_registry_values(
        "CapabilityUpdateReason",
        common["CapabilityUpdateReason"]["values"],
        update_values,
        "documents 02 and 05",
    )

    format_section = extract_section(doc05, r"### 4\.2 .*Formats")
    source_values = values_from_value_name_table(
        table_after(format_section, r"SourceFormat est", ("Valeur", "Nom"))
    )
    delivered_table = table_after(format_section, r"DeliveredFormat est", ("Valeur", "Nom", "Bit de support"))
    delivered_values = values_from_value_name_table(delivered_table)
    delivered_bits: list[dict[str, object]] = []
    for raw_row in delivered_table.rows:
        row = row_dict(delivered_table, raw_row)
        enum_value = parse_exact_int(cell(row, "Valeur"))
        name = upper_snake(cell(row, "Nom"))
        raw_mask = strip_inline_markdown(cell(row, "Bit de support"))
        if enum_value == 0:
            if keyify(raw_mask) != "aucun":
                raise SchemaError("DeliveredFormat.INVALID must have no support bit")
            continue
        mask = parse_exact_int(raw_mask)
        if mask != 1 << (enum_value - 1):
            raise SchemaError(f"DeliveredFormatBit {name} does not match enum value {enum_value}")
        delivered_bits.append({"name": name, "value": mask, "enum_value": enum_value})
    alpha_values = values_from_value_name_table(
        table_after(format_section, r"AlphaMode est", ("Valeur", "Nom"))
    )
    timing_values = values_from_value_name_table(
        table_after(format_section, r"AssetTimingMode est", ("Valeur", "Nom"))
    )
    for name, values in (
        ("SourceFormat", source_values),
        ("DeliveredFormat", delivered_values),
        ("AlphaMode", alpha_values),
        ("AssetTimingMode", timing_values),
    ):
        assert_same_registry_values(name, common[name]["values"], values, "documents 04 and 05")

    registries["DeliveredFormatBit"] = make_bitmask_registry(
        "DeliveredFormatBit",
        32,
        delivered_bits,
        registry_source("05-capabilities-et-vues-specialisees.md", "4.2"),
        extensible=True,
    )

    manifest_match = re.search(
        r"ManifestFlags est un bitmap [^:]+\s*:\s*bit 0, masque (0[xX][0-9a-fA-F]+), "
        r"([A-Z][A-Z0-9_]*)\s*;\s*bit 1, masque (0[xX][0-9a-fA-F]+), ([A-Z][A-Z0-9_]*)",
        format_section,
    )
    if manifest_match is None:
        raise SchemaError("ManifestFlags assignments not found in document 05")
    manifest_values = [
        {"name": manifest_match.group(2), "value": int(manifest_match.group(1), 0), "bit": 0},
        {"name": manifest_match.group(4), "value": int(manifest_match.group(3), 0), "bit": 1},
    ]
    assert_same_registry_values(
        "ManifestFlags", common["ManifestFlags"]["values"], manifest_values, "documents 04 and 05"
    )

    comm_section = extract_section(doc05, r"### 5\.1 .*Enums")
    comm_table = find_table(comm_section, ("Enum", "Valeur", "Nom"))
    comm_values: dict[str, list[dict[str, object]]] = {}
    for raw_row in comm_table.rows:
        row = row_dict(comm_table, raw_row)
        name = strip_inline_markdown(cell(row, "Enum"))
        comm_values.setdefault(name, []).append(
            {"name": upper_snake(cell(row, "Nom")), "value": parse_exact_int(cell(row, "Valeur"))}
        )
    for name, values in comm_values.items():
        if name not in common:
            raise SchemaError(f"specialized communication enum {name} missing from document 04")
        assert_same_registry_values(name, common[name]["values"], values, "documents 04 and 05")

    video_section = extract_section(doc05, r"### 6\.1 .*Enums et bitmaps")
    video_config_values = values_from_value_name_table(
        table_after(video_section, r"VideoConfigResult\s*:", ("Valeur", "Nom"))
    )
    codec_values = values_from_value_name_table(
        table_after(video_section, r"Codec\s*:", ("Valeur", "Nom"))
    )
    registries["VideoConfigResult"] = make_enum_registry(
        "VideoConfigResult",
        8,
        video_config_values,
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
    )
    registries["Codec"] = make_enum_registry(
        "Codec", 8, codec_values, registry_source("05-capabilities-et-vues-specialisees.md", "6.1")
    )

    profile_table = table_after(
        video_section, r"H264Profile utilise", ("Valeur config", "Nom", "Bit de supported_h264_profiles")
    )
    profile_values = values_from_value_name_table(profile_table, value_header="Valeur config")
    profile_bits: list[dict[str, object]] = []
    for raw_row in profile_table.rows:
        row = row_dict(profile_table, raw_row)
        profile_bits.append(
            {
                "name": upper_snake(cell(row, "Nom")),
                "value": parse_exact_int(cell(row, "Bit de supported_h264_profiles")),
                "enum_value": parse_exact_int(cell(row, "Valeur config")),
            }
        )
    registries["H264Profile"] = make_enum_registry(
        "H264Profile", 8, profile_values, registry_source("05-capabilities-et-vues-specialisees.md", "6.1")
    )
    registries["H264ProfileBit"] = make_bitmask_registry(
        "H264ProfileBit",
        32,
        profile_bits,
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        extensible=True,
    )

    level_table = table_after(
        video_section, r"H264Level utilise", ("Valeur config", "Nom", "Bit de supported_h264_levels")
    )
    level_values = values_from_value_name_table(level_table, value_header="Valeur config")
    level_bits: list[dict[str, object]] = []
    for raw_row in level_table.rows:
        row = row_dict(level_table, raw_row)
        level_bits.append(
            {
                "name": upper_snake(cell(row, "Nom")),
                "value": parse_exact_int(cell(row, "Bit de supported_h264_levels")),
                "enum_value": parse_exact_int(cell(row, "Valeur config")),
            }
        )
    registries["H264Level"] = make_enum_registry(
        "H264Level", 8, level_values, registry_source("05-capabilities-et-vues-specialisees.md", "6.1")
    )
    registries["H264LevelBit"] = make_bitmask_registry(
        "H264LevelBit",
        32,
        level_bits,
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        extensible=True,
    )

    video_enum_table = table_after(
        video_section, r"Les bits [^\n]+bitmaps profil et niveau", ("Enum", "Valeur", "Nom")
    )
    grouped_video_enums: dict[str, list[dict[str, object]]] = {}
    for raw_row in video_enum_table.rows:
        row = row_dict(video_enum_table, raw_row)
        name = strip_inline_markdown(cell(row, "Enum"))
        grouped_video_enums.setdefault(name, []).append(
            {"name": upper_snake(cell(row, "Nom")), "value": parse_exact_int(cell(row, "Valeur"))}
        )
    for name, values in grouped_video_enums.items():
        registries[name] = make_enum_registry(
            name, 8, values, registry_source("05-capabilities-et-vues-specialisees.md", "6.1")
        )

    render_table = table_after(video_section, r"RenderProfileBit\s*:", ("Bit", "Masque", "Profil"))
    render_values = values_from_bit_table(
        render_table, value_header="Masque", name_header="Profil"
    )
    registries["RenderProfileBit"] = make_bitmask_registry(
        "RenderProfileBit",
        32,
        render_values,
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        extensible=True,
    )

    overlay_table = table_after(video_section, r"OverlayCapabilityBit\s*:", ("Bit", "Masque", "Overlay local"))
    overlay_values = values_from_bit_table(
        overlay_table, value_header="Masque", name_header="Overlay local"
    )
    overlay_registry = make_bitmask_registry(
        "OverlayCapabilityBit",
        32,
        overlay_values,
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        extensible=True,
        notes=("Bits 0 through 3 are mandatory for OverlayMode.CLIENT in v1",),
    )
    overlay_registry["required_mask"] = sum(int(item["value"]) for item in overlay_values if int(item["bit"]) < 4)
    registries["OverlayCapabilityBit"] = overlay_registry

    frame_table = table_after(video_section, r"VideoFrameFlag\s*:", ("Bit", "Masque", "Nom"))
    registries["VideoFrameFlags"] = make_bitmask_registry(
        "VideoFrameFlags",
        16,
        values_from_bit_table(frame_table, value_header="Masque"),
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
    )

    keyframe_table = table_after(video_section, r"VideoKeyframeReason\s*:", ("Valeur", "Nom"))
    registries["VideoKeyframeReason"] = make_enum_registry(
        "VideoKeyframeReason",
        8,
        values_from_value_name_table(keyframe_table),
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        reserved_ranges=((0, 0),),
    )

    stop_table = table_after(
        video_section, r"VideoStopReason\s*:", ("Valeur", "Nom", "metteur")
    )
    registries["VideoStopReason"] = make_enum_registry(
        "VideoStopReason",
        8,
        values_from_value_name_table(stop_table, metadata_headers=("metteur",)),
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
        reserved_ranges=((0, 0),),
    )

    stop_flag_match = re.search(
        r"VideoStopFlag bit ([0-9]+), masque (0[xX][0-9a-fA-F]+), signifie ([A-Z][A-Z0-9_]*)",
        video_section,
    )
    if stop_flag_match is None:
        raise SchemaError("VideoStopFlag normative assignment not found")
    registries["VideoStopFlags"] = make_bitmask_registry(
        "VideoStopFlags",
        8,
        [
            {
                "name": stop_flag_match.group(3),
                "bit": int(stop_flag_match.group(1)),
                "value": int(stop_flag_match.group(2), 0),
            }
        ],
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
    )

    stats_table = table_after(video_section, r"VideoStatsFlag\s*:", ("Bit", "Masque", "Nom"))
    registries["VideoStatsFlags"] = make_bitmask_registry(
        "VideoStatsFlags",
        16,
        values_from_bit_table(stats_table, value_header="Masque"),
        registry_source("05-capabilities-et-vues-specialisees.md", "6.1"),
    )
    return registries


def parse_unknown_value_policy(doc06: str) -> list[str]:
    section = extract_section(doc06, r"## 8\. Types inconnus et extensions")
    rules = [strip_inline_markdown(line[2:]) for line in section.splitlines() if line.startswith("- ")]
    if len(rules) != 7:
        raise SchemaError(f"unknown-value policy must contain exactly 7 normative rules, got {len(rules)}")
    return rules


def parse_scalar_types(doc02: str) -> list[dict[str, object]]:
    section = extract_section(doc02, r"### 2\.1 Types primitifs")
    table = find_table(section, ("Notation", "Taille", "Encodage"))
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        result.append(
            {
                "name": strip_inline_markdown(cell(row, "Notation")),
                "size": strip_inline_markdown(cell(row, "Taille")),
                "encoding": strip_inline_markdown(cell(row, "Encodage")),
            }
        )
    require_unique(result, "name", "scalar type")
    return result


def wire_layout(table: MarkdownTable) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        result.append(
            {
                "field": strip_inline_markdown(cell(row, "Champ")),
                "wire": strip_inline_markdown(cell(row, "Type")),
                "rule": strip_inline_markdown(cell(row, "gle")),
            }
        )
    require_unique(result, "field", "wire layout")
    return result


def parse_wire_conventions(doc01: str, doc02: str, doc03: str) -> dict[str, object]:
    primitive_section = extract_section(doc02, r"### 2\.1 .*Types primitifs")
    if not re.search(r"bool\S*en filaire.*?`u8`.*?`0`.*?`1`", primitive_section, re.DOTALL):
        raise SchemaError("bool8 closed encoding rule is missing")
    if not re.search(r"`float32` non fini.*?rejet", primitive_section, re.DOTALL):
        raise SchemaError("float32 non-finite rejection rule is missing")
    if "0x00000000" not in primitive_section or "-0.0" not in primitive_section:
        raise SchemaError("float32 zero canonicalization rule is missing")

    array_section = extract_section(doc02, r"### 2\.2 .*tableaux")
    array_tables = parse_tables(array_section)
    if len(array_tables) != 3:
        raise SchemaError(f"string/list section must contain exactly three layouts, got {len(array_tables)}")

    absence_section = extract_section(doc02, r"### 2\.3 .*Valeurs absentes")
    absence_rules = [strip_inline_markdown(line[2:]) for line in absence_section.splitlines() if line.startswith("- ")]
    optionality_section = extract_section(doc01, r"### 7\.2 .*Optionalit.*")
    optionality_mechanisms = [
        strip_inline_markdown(match.group(1))
        for match in re.finditer(r"^[1-5]\.\s+(.+)$", optionality_section, re.MULTILINE)
    ]
    if len(absence_rules) != 4 or len(optionality_mechanisms) != 5:
        raise SchemaError("absence/optionality mechanism inventory is incomplete")

    units_section = extract_section(doc02, r"### 2\.4 .*Unit.*rep.*")
    unit_rules = [strip_inline_markdown(line[2:]) for line in units_section.splitlines() if line.startswith("- ")]
    if len(unit_rules) != 7:
        raise SchemaError(f"unit/frame section must contain seven rules, got {len(unit_rules)}")
    invariant_units = extract_section(doc01, r"### 7\.1 .*Rep.*unit.*")
    invariant_rules = [strip_inline_markdown(line[2:]) for line in invariant_units.splitlines() if line.startswith("- ")]
    if len(invariant_rules) != 8:
        raise SchemaError(f"public unit invariant section must contain eight rules, got {len(invariant_rules)}")

    clock_section = extract_section(doc03, r"### 6\.1 .*Domaines")
    monotonic_rules = [strip_inline_markdown(line[2:]) for line in clock_section.splitlines() if line.startswith("- ")]
    if len(monotonic_rules) != 4:
        raise SchemaError("monotonic clock domain must contain four properties")
    mission_match = re.search(r"`mission_time_us`\s+(.+)", clock_section)
    if mission_match is None:
        raise SchemaError("mission clock domain rule is missing")

    classification_section = extract_section(doc01, r"## 11\. .*classification")
    classifications: list[dict[str, str]] = []
    for line in classification_section.splitlines():
        match = re.match(r"- \*\*([^*]+)\*\*\s*:\s*(.+)", line)
        if match:
            classifications.append(
                {"category": keyify(match.group(1)), "label": match.group(1), "rule": strip_inline_markdown(match.group(2))}
            )
    if len(classifications) != 5:
        raise SchemaError("limit classification must contain exactly five categories")
    if "42042" not in classification_section:
        raise SchemaError("implementation-default port classification is missing")

    return {
        "bool8": {
            "wire": "u8",
            "false": 0,
            "true": 1,
            "other_values": "reject",
            "source": registry_source("02-format-filaire-et-registres.md", "2.1"),
        },
        "opaque_bytes": {
            "notation": "bytes[N]",
            "size": "N octets",
            "encoding": "opaque bytes in declared order",
            "implicit_padding": False,
            "source": registry_source("02-format-filaire-et-registres.md", "2.1"),
        },
        "string": {
            "notation": "str<N>",
            "layout": wire_layout(array_tables[0]),
            "length_unit": "bytes",
            "encoding": "UTF-8",
            "nul_terminator": False,
            "embedded_nul_default": "forbidden",
            "invalid_utf8": "reject",
            "source": registry_source("02-format-filaire-et-registres.md", "2.2"),
        },
        "list": {
            "notation": "list<T,N>",
            "layout": wire_layout(array_tables[1]),
            "item_size": "fixed and common to all items",
            "padding": False,
            "overflow_checks": ("count * item_size before allocation",),
            "source": registry_source("02-format-filaire-et-registres.md", "2.2"),
        },
        "vlist": {
            "notation": "vlist<T,N>",
            "layout": wire_layout(array_tables[2]),
            "item_size": "variable and carried per item",
            "padding": False,
            "overflow_checks": ("each item bound", "total sum"),
            "autodetection": False,
            "source": registry_source("02-format-filaire-et-registres.md", "2.2"),
        },
        "absence": {
            "implicit_null": False,
            "wire_rules": absence_rules,
            "allowed_mechanisms": optionality_mechanisms,
            "implicit_sentinels_forbidden": ("NaN", "infinity", "magic string", "max integer", "truncation"),
            "sources": (
                registry_source("02-format-filaire-et-registres.md", "2.3"),
                registry_source("01-cadre-normatif-et-perimetre.md", "7.2"),
            ),
        },
        "float32_canonicalization": {
            "non_finite": "reject unless field explicitly permits it; no v1 field does",
            "emitted_zero_bits": "0x00000000",
            "accepted_negative_zero": True,
            "public_zero": "+0.0",
            "source": registry_source("02-format-filaire-et-registres.md", "2.1"),
        },
        "units_and_frame": {
            "network_time_unit": "integer microseconds",
            "position_distance_unit": "FSO world-unit",
            "linear_velocity_unit": "FSO world-unit per second",
            "angle_unit": "radian",
            "angular_velocity_unit": "radian per second",
            "handedness": "right-handed",
            "axes": {"x": "right", "y": "up", "z": "forward"},
            "normative_rules": unit_rules,
            "public_invariants": invariant_rules,
            "sources": (
                registry_source("02-format-filaire-et-registres.md", "2.4"),
                registry_source("01-cadre-normatif-et-perimetre.md", "7.1"),
            ),
        },
        "quaternion": {
            "wire": "float32[4]",
            "order": ("w", "x", "y", "z"),
            "mapping": "local_to_world",
            "normalized_before_emit": True,
            "canonical_sign": "w >= 0; when w == 0, first non-zero of x,y,z is positive",
            "source": registry_source("02-format-filaire-et-registres.md", "2.4"),
        },
        "clock_domains": {
            "process_monotonic": {
                "fields": ("sent_time_us", "producer_sample_time_us"),
                "properties": monotonic_rules,
                "unit": "microseconds",
            },
            "mission_simulation": {
                "field": "mission_time_us",
                "properties": strip_inline_markdown(mission_match.group(1)),
                "not_for": ("datagram ordering", "timeouts", "clock synchronization"),
            },
            "source": registry_source("03-session-horloges-fiabilite.md", "6.1"),
        },
        "limit_classification": {
            "categories": classifications,
            "classified_examples": (
                {
                    "name": "telemetry_port",
                    "value": 42042,
                    "category": "defaut_d_implementation",
                    "configurable": True,
                },
            ),
            "source": registry_source("01-cadre-normatif-et-perimetre.md", "11"),
        },
    }


def parse_datagram_header(doc02: str) -> list[dict[str, object]]:
    section = extract_section(doc02, r"### 4\.2 Layout exact")
    table = find_table(section, ("Offset", "Champ", "Type", "Règle"))
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        result.append(
            {
                "offset": parse_exact_int(cell(row, "Offset")),
                "name": strip_inline_markdown(cell(row, "Champ")),
                "wire": strip_inline_markdown(cell(row, "Type")),
                "rule": strip_inline_markdown(cell(row, "Règle")),
            }
        )
    require_unique(result, "offset", "datagram header")
    require_unique(result, "name", "datagram header")
    if [int(item["offset"]) for item in result] != sorted(int(item["offset"]) for item in result):
        raise SchemaError("datagram header offsets regress")
    return result


def named_constant_table(section: str, name_header: str, value_header: str) -> list[dict[str, object]]:
    table = find_table(section, (name_header, value_header))
    result: list[dict[str, object]] = []
    for raw_row in table.rows:
        row = row_dict(table, raw_row)
        raw_name = strip_inline_markdown(cell(row, name_header))
        raw_value = strip_inline_markdown(cell(row, value_header))
        item: dict[str, object] = {"name": raw_name, "raw_value": raw_value}
        numeric = first_numeric_value(raw_value)
        if numeric is not None:
            item["numeric_value"] = numeric
        result.append(item)
    require_unique(result, "name", f"constant table {name_header}")
    return result


def parse_constants(doc02: str, doc03: str, doc05: str, doc06: str) -> dict[str, object]:
    version_section = extract_section(doc02, r"## 3\. Version du protocole")
    version_table = find_table(version_section, ("Élément", "Valeur"))
    version_values: dict[str, int] = {}
    for raw_row in version_table.rows:
        row = row_dict(version_table, raw_row)
        version_values[keyify(cell(row, "Élément"))] = parse_exact_int(cell(row, "Valeur"))
    expected_version_keys = {"version_major", "version_minor", "header_size"}
    if set(version_values) != expected_version_keys:
        raise SchemaError(f"unexpected protocol version keys: {version_values}")

    constexpr_values = {
        name: int(value)
        for name, value in re.findall(r"constexpr\s+std::size_t\s+([A-Z0-9_]+)\s*=\s*([0-9]+)\s*;", doc02)
    }
    required_cpp_constants = {
        "TELEMETRY_MAX_DATAGRAM_SIZE",
        "TELEMETRY_HEADER_SIZE_V1",
        "TELEMETRY_MAX_FRAGMENT_PAYLOAD",
    }
    if set(constexpr_values) != required_cpp_constants:
        raise SchemaError(f"wire constexpr set drifted: {constexpr_values}")

    magic_section = extract_section(doc02, r"### 4\.1 Magic et taille")
    magic_table = find_table(magic_section, ("Représentation", "Valeur"))
    magic_bytes = None
    magic_u32 = None
    for raw_row in magic_table.rows:
        row = row_dict(magic_table, raw_row)
        representation = keyify(cell(row, "Représentation"))
        value = strip_inline_markdown(cell(row, "Valeur"))
        if "octets" in representation:
            magic_bytes = value.lower()
        elif "u32" in representation:
            magic_u32 = parse_exact_int(value)
    if magic_bytes is None or magic_u32 is None:
        raise SchemaError("magic byte/u32 representations are incomplete")

    crc_section = extract_section(doc02, r"## 5\. Contrôles d'intégrité")
    crc_table = named_constant_table(crc_section, "Paramètre", "Valeur")

    transaction_section = extract_section(doc02, r"### 10\.1 Transaction paginée")
    transaction_constants = named_constant_table(transaction_section, "Constante", "Valeur")

    reassembly_section = extract_section(doc02, r"### 11\.4 Limites et quotas de réassemblage")
    reassembly_table = find_table(
        reassembly_section,
        ("Classe", "Taille logique max", "Fragments max", "Réassemblages simultanés/client", "Octets réservés/client"),
    )
    reassembly: list[dict[str, object]] = []
    for raw_row in reassembly_table.rows:
        row = row_dict(reassembly_table, raw_row)
        reassembly.append(
            {
                "class": strip_inline_markdown(cell(row, "Classe")),
                "max_message_size": first_numeric_value(cell(row, "Taille logique max")),
                "max_fragments": first_numeric_value(cell(row, "Fragments max")),
                "max_concurrent_per_client": first_numeric_value(cell(row, "Réassemblages simultanés/client")),
                "reserved_bytes_per_client": first_numeric_value(cell(row, "Octets réservés/client")),
            }
        )
    if len(reassembly) != 2 or any(None in item.values() for item in reassembly):
        raise SchemaError(f"invalid reassembly limits: {reassembly}")

    reliability_section = extract_section(doc03, r"### 8\.1 Constantes")
    reliability_constants = named_constant_table(reliability_section, "Constante", "Valeur v1.0")

    video_section = extract_section(doc05, r"## 9\. Fragmentation et récupération vidéo")
    video_constants = named_constant_table(video_section, "Limite", "Valeur v1")

    resource_section = extract_section(doc06, r"### 9\.5 Limites de ressources")
    resource_table = find_table(resource_section, ("Ressource", "État/fiable", "Vidéo"))
    resource_rows = [row_dict(resource_table, raw_row) for raw_row in resource_table.rows]
    if len(resource_rows) < 2:
        raise SchemaError("resource cross-check table is incomplete")
    state_size = first_numeric_value(cell(resource_rows[0], "État/fiable"))
    video_size = first_numeric_value(cell(resource_rows[0], "Vidéo"))
    state_fragments = first_numeric_value(cell(resource_rows[1], "État/fiable"))
    video_fragments = first_numeric_value(cell(resource_rows[1], "Vidéo"))
    if (state_size, video_size, state_fragments, video_fragments) != (
        reassembly[0]["max_message_size"],
        reassembly[1]["max_message_size"],
        reassembly[0]["max_fragments"],
        reassembly[1]["max_fragments"],
    ):
        raise SchemaError("resource maxima drift between documents 02 and 06")

    core = {
        "magic_ascii": "FSTL",
        "magic_bytes_hex": magic_bytes.replace(" ", ""),
        "magic_u32": magic_u32,
        "version_major": version_values["version_major"],
        "version_minor": version_values["version_minor"],
        "header_size": version_values["header_size"],
        "max_datagram_size": constexpr_values["TELEMETRY_MAX_DATAGRAM_SIZE"],
        "max_fragment_payload": constexpr_values["TELEMETRY_MAX_FRAGMENT_PAYLOAD"],
        "endianness": "little-endian",
    }
    if constexpr_values["TELEMETRY_HEADER_SIZE_V1"] != core["header_size"]:
        raise SchemaError("header size drifts between the version and constexpr tables")
    if int(core["header_size"]) + int(core["max_fragment_payload"]) != int(core["max_datagram_size"]):
        raise SchemaError("header_size + max_fragment_payload must equal max_datagram_size")

    for item in reassembly:
        canonical_fragments = math.ceil(int(item["max_message_size"]) / int(core["max_fragment_payload"]))
        if canonical_fragments > int(item["max_fragments"]):
            raise SchemaError(f"fragment cap cannot represent maximum {item['class']} message")
        expected_bytes = int(item["max_message_size"]) * int(item["max_concurrent_per_client"])
        if expected_bytes != int(item["reserved_bytes_per_client"]):
            raise SchemaError(f"reassembly byte quota drift for {item['class']}")

    transaction_by_name = {str(item["name"]): item for item in transaction_constants}
    transaction_size = transaction_by_name.get("TELEMETRY_MAX_TRANSACTION_SIZE", {}).get("numeric_value")
    transaction_count = transaction_by_name.get("TELEMETRY_MAX_CANDIDATE_TRANSACTIONS_PER_CLIENT", {}).get("numeric_value")
    transaction_bytes = transaction_by_name.get("TELEMETRY_MAX_CANDIDATE_TRANSACTION_BYTES_PER_CLIENT", {}).get(
        "numeric_value"
    )
    if not all(isinstance(value, int) for value in (transaction_size, transaction_count, transaction_bytes)):
        raise SchemaError("required transaction constants are missing numeric values")
    if int(transaction_size) * int(transaction_count) != int(transaction_bytes):
        raise SchemaError("candidate transaction byte budget does not match size * count")

    return {
        "core": core,
        "crc32_iso_hdlc": crc_table,
        "transactions": transaction_constants,
        "reassembly": reassembly,
        "reliability": reliability_constants,
        "video_fragmentation": video_constants,
    }


def evaluate_cpp_integer(expression: str, known: dict[str, int]) -> int:
    cleaned = expression.replace("'", "")
    cleaned = re.sub(r"(?<=[0-9a-fA-F])(?:ULL|LLU|UL|LU|U|L)\b", "", cleaned, flags=re.IGNORECASE)
    try:
        tree = ast.parse(cleaned, mode="eval")
    except SyntaxError as exc:
        raise SchemaError(f"unsupported C++ integer expression {expression!r}") from exc

    def visit(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return visit(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return int(node.value)
        if isinstance(node, ast.Name):
            if node.id not in known:
                raise SchemaError(f"unknown C++ constant {node.id!r} in {expression!r}")
            return known[node.id]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub, ast.Invert)):
            value = visit(node.operand)
            if isinstance(node.op, ast.UAdd):
                return value
            if isinstance(node.op, ast.USub):
                return -value
            return ~value
        if isinstance(node, ast.BinOp) and isinstance(
            node.op, (ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd)
        ):
            left = visit(node.left)
            right = visit(node.right)
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.Mult):
                return left * right
            if isinstance(node.op, ast.FloorDiv):
                return left // right
            if isinstance(node.op, ast.LShift):
                return left << right
            if isinstance(node.op, ast.RShift):
                return left >> right
            if isinstance(node.op, ast.BitOr):
                return left | right
            return left & right
        raise SchemaError(f"unsupported AST in C++ integer expression {expression!r}: {ast.dump(node)}")

    return visit(tree)


def parse_cpp_protocol_constants() -> tuple[
    dict[str, dict[str, object]], dict[str, int], dict[str, bool], dict[str, dict[str, str]]
]:
    try:
        text = CPP_CONSTANTS_PATH.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise SchemaError(f"cannot read C++ protocol constants {CPP_CONSTANTS_PATH}: {exc}") from exc

    enums: dict[str, dict[str, object]] = {}
    enum_pattern = re.compile(
        r"enum\s+(?:class\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*std::uint(8|16|32|64)_t\s*\{(.*?)\};",
        re.DOTALL,
    )
    for match in enum_pattern.finditer(text):
        name = match.group(1)
        width_bits = int(match.group(2))
        members: list[dict[str, object]] = []
        local_values: dict[str, int] = {}
        body = re.sub(r"//[^\n]*", "", match.group(3))
        for raw_member in body.split(","):
            member = raw_member.strip()
            if not member:
                continue
            assignment = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)", member, re.DOTALL)
            if assignment is None:
                raise SchemaError(f"C++ enum {name} has an implicit or malformed member {member!r}")
            member_name = assignment.group(1)
            value = evaluate_cpp_integer(assignment.group(2).strip(), local_values)
            members.append({"name": member_name, "value": value})
            local_values[member_name] = value
        require_unique(members, "name", f"C++ enum {name}")
        require_unique(members, "value", f"C++ enum {name}")
        if name in enums:
            raise SchemaError(f"duplicate C++ enum {name}")
        enums[name] = {"width_bits": width_bits, "values": members}

    constants: dict[str, int] = {}
    constant_pattern = re.compile(
        r"constexpr\s+(?:std::uint(?:8|16|32|64)_t|std::size_t)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);"
    )
    for match in constant_pattern.finditer(text):
        name = match.group(1)
        if name in constants:
            raise SchemaError(f"duplicate C++ numeric constant {name}")
        constants[name] = evaluate_cpp_integer(match.group(2), constants)

    bool_constants = {
        name: raw_value == "true"
        for name, raw_value in re.findall(
            r"constexpr\s+bool\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(true|false)\s*;", text
        )
    }
    typed_aliases: dict[str, dict[str, str]] = {}
    for type_name, alias_name, target_name in re.findall(
        r"constexpr\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*;",
        text,
    ):
        if type_name == "bool":
            continue
        typed_aliases[alias_name] = {"type": type_name, "target": target_name}
    return enums, constants, bool_constants, typed_aliases


def cpp_registry_bindings() -> dict[str, dict[str, object]]:
    bindings: dict[str, dict[str, object]] = {
        "MessageType": {"enum": "MessageType"},
        "MessageFlags": {
            "enum": "MessageFlag",
            "prefix": "MessageFlag",
            "known": "KnownMessageFlags",
            "reserved": "ReservedMessageFlags",
        },
        "RecordType": {"enum": "RecordType"},
        "RecordFlags": {
            "enum": "RecordFlag",
            "prefix": "RecordFlag",
            "known": "KnownRecordFlags",
            "reserved": "ReservedRecordFlags",
        },
        "Capability": {
            "enum": "Capability",
            "prefix": "Capability",
            "known": "KnownCapabilities",
            "reserved": "ReservedCapabilities",
            "aliases": {"UPDATE": "CAPABILITY_UPDATE"},
        },
        "CapabilityExtensionType": {"enum": "CapabilityExtensionType"},
        "WelcomeStatus": {"enum": "WelcomeStatus"},
        "SessionBeginFlags": {
            "enum": "SessionBeginFlag",
            "prefix": "SessionBeginFlag",
            "known": "KnownSessionBeginFlags",
            "reserved": "ReservedSessionBeginFlags",
        },
        "HeartbeatKind": {"enum": "HeartbeatKind"},
        "ResyncRequestFlags": {
            "enum": "ResyncRequestFlag",
            "prefix": "ResyncRequestFlag",
            "known": "KnownResyncRequestFlags",
            "reserved": "ReservedResyncRequestFlags",
        },
        "SessionEndReason": {"enum": "SessionEndReason"},
        "SessionEndFlags": {
            "enum": "SessionEndFlag",
            "prefix": "SessionEndFlag",
            "known": "KnownSessionEndFlags",
            "reserved": "ReservedSessionEndFlags",
        },
        "CapabilityUpdateReason": {"enum": "CapabilityUpdateReason"},
        "ManifestKind": {"enum": "ManifestKind"},
        "SnapshotFlags": {
            "enum": "SnapshotFlag",
            "prefix": "SnapshotFlag",
            "known": "KnownSnapshotFlags",
            "reserved": "ReservedSnapshotFlags",
        },
        "EventDeliveryClass": {"enum": "EventDeliveryClass"},
        "EventKind": {"enum": "EventKind"},
        "EventReasonCode": {"enum": "EventReasonCode"},
        "CommNegotiationResult": {"enum": "CommNegotiationResult"},
        "DeliveredFormatBit": {
            "enum": "DeliveredFormatBit",
            "prefix": "DeliveredFormatBit",
            "known": "KnownDeliveredFormatBits",
            "reserved": "ReservedDeliveredFormatBits",
        },
        "ManifestFlags": {
            "enum": "ManifestFlag",
            "prefix": "ManifestFlag",
            "known": "KnownManifestFlags",
            "reserved": "ReservedManifestFlags",
        },
        "VideoConfigResult": {"enum": "VideoConfigResult"},
        "Codec": {"enum": "Codec"},
        "H264Profile": {"enum": "H264Profile"},
        "H264ProfileBit": {
            "enum": "H264ProfileBit",
            "prefix": "H264ProfileBit",
            "known": "KnownH264ProfileBits",
            "reserved": "ReservedH264ProfileBits",
        },
        "H264Level": {"enum": "H264Level"},
        "H264LevelBit": {
            "enum": "H264LevelBit",
            "prefix": "H264LevelBit",
            "known": "KnownH264LevelBits",
            "reserved": "ReservedH264LevelBits",
        },
        "PixelFormat": {"enum": "PixelFormat"},
        "RenderProfile": {"enum": "RenderProfile"},
        "RenderProfileBit": {
            "enum": "RenderProfileBit",
            "prefix": "RenderProfileBit",
            "known": "KnownRenderProfileBits",
            "reserved": "ReservedRenderProfileBits",
        },
        "OverlayMode": {"enum": "OverlayMode"},
        "OverlayCapabilityBit": {
            "enum": "OverlayCapabilityBit",
            "prefix": "OverlayCapabilityBit",
            "known": "KnownOverlayCapabilityBits",
            "reserved": "ReservedOverlayCapabilityBits",
        },
        "RecoveryMode": {"enum": "RecoveryMode"},
        "VideoFrameFlags": {
            "enum": "VideoFrameFlag",
            "prefix": "VideoFrameFlag",
            "known": "KnownVideoFrameFlags",
            "reserved": "ReservedVideoFrameFlags",
        },
        "VideoKeyframeReason": {"enum": "VideoKeyframeReason"},
        "VideoStopReason": {"enum": "VideoStopReason"},
        "VideoStopFlags": {
            "enum": "VideoStopFlag",
            "prefix": "VideoStopFlag",
            "known": "KnownVideoStopFlags",
            "reserved": "ReservedVideoStopFlags",
        },
        "VideoStatsFlags": {
            "enum": "VideoStatsFlag",
            "prefix": "VideoStatsFlag",
            "known": "KnownVideoStatsFlags",
            "reserved": "ReservedVideoStatsFlags",
        },
        "ValidationError": {"enum": "ValidationError"},
        "VisibilityMode": {"enum": "VisibilityMode"},
        "AckFlags": {
            "enum": "AckFlag",
            "known": "KnownAckFlags",
            "reserved": "ReservedAckFlags",
        },
        "NackReason": {"enum": "NackReason"},
        "ResyncReason": {"enum": "ResyncReason"},
    }
    direct_common_enums = (
        "AuthorityMode",
        "SessionPhase",
        "MissionPhase",
        "ObjectType",
        "LifecyclePhase",
        "TransitMode",
        "ControlMode",
        "WeaponSubtype",
        "SubsystemType",
        "AnimationState",
        "EtsMode",
        "WeaponFamily",
        "ValueTrend",
        "RadarMode",
        "SensorState",
        "RadarVisibility",
        "RadarCategory",
        "ThreatLevel",
        "GuidanceType",
        "ScanPhase",
        "DisclosureState",
        "DockingPhase",
        "SupportPhase",
        "NavPointType",
        "AutopilotState",
        "AutopilotRefusal",
        "SourceFormat",
        "DeliveredFormat",
        "AlphaMode",
        "AssetTimingMode",
        "CommEventKind",
        "CommPlaybackMode",
        "CommColorMode",
        "CommStopReason",
        "HudAlertMissileLockState",
        "HudAlertWarningKind",
    )
    for name in direct_common_enums:
        bindings[name] = {"enum": name}

    bitmask_bindings = (
        ("EntityLifecycleFlags", "EntityLifecycleFlag"),
        ("ShipRoleFlags", "ShipRoleFlag"),
        ("SensorVisibilityFlags", "SensorVisibilityFlag"),
        ("ProtectionFlags", "ProtectionFlag"),
        ("PhysicsModeFlags", "PhysicsModeFlag"),
        ("ControlFlags", "ControlFlag"),
        ("SubsystemFlags", "SubsystemFlag"),
        ("WeaponGlobalFlags", "WeaponGlobalFlag"),
        ("WeaponClassFlags", "WeaponClassFlag"),
        ("WeaponEffectFlags", "WeaponEffectFlag"),
        ("ClassSubsystemStaticFlags", "ClassSubsystemStaticFlag"),
        ("ContactFlags", "ContactFlag"),
        ("EffectFlags", "EffectFlag"),
        ("EventFamilyBits", "EventFamilyBit"),
        ("StateDomainCoverage", "StateDomainCoverageBit"),
        ("PropulsionFlags", "PropulsionFlag"),
        ("CountermeasureStateFlags", "CountermeasureStateFlag"),
        ("ScanValidityFlags", "ScanValidityFlag"),
        ("SupportFlags", "SupportFlag"),
        ("NavPointFlags", "NavPointFlag"),
        ("EventFlags", "EventFlag"),
    )
    for registry_name, enum_name in bitmask_bindings:
        stem = enum_name
        if enum_name.endswith("Bit"):
            constant_stem = enum_name + "s"
        elif enum_name.endswith("Flag"):
            constant_stem = enum_name + "s"
        else:
            constant_stem = enum_name
        bindings[registry_name] = {
            "enum": enum_name,
            "prefix": enum_name,
            "known": f"Known{constant_stem}",
            "reserved": f"Reserved{constant_stem}",
        }
    bindings["StateDomainCoverage"]["known"] = "KnownStateDomainCoverageBitsV1_1"
    bindings["StateDomainCoverage"]["reserved"] = "ReservedStateDomainCoverageBitsV1_1"
    presence_registries = (
        "CargoScanStatePresence",
        "ClassBankPresence",
        "ClassManifestPresence",
        "ClassSubsystemPresence",
        "ControlStatePresence",
        "CountermeasureStatePresence",
        "DamageStatePresence",
        "DockingStatePresence",
        "EffectStatePresence",
        "EnergyStatePresence",
        "EntityLifecyclePresence",
        "EventItemPresence",
        "FlightStatePresence",
        "IncomingMissilePresence",
        "LockItemPresence",
        "LockStatePresence",
        "MissionStatePresence",
        "NavPointPresence",
        "NavigationStatePresence",
        "PrimaryBankPresence",
        "PropulsionStatePresence",
        "RadarContactsPresence",
        "RadarStatePresence",
        "SecondaryBankPresence",
        "SessionStatePresence",
        "ShieldStatePresence",
        "ShipIdentityPresence",
        "SubsystemAnimationPresence",
        "SubsystemStatePresence",
        "SupportStatePresence",
        "TargetStatePresence",
        "ThreatStatePresence",
        "HudAlertStatePresence",
        "TurretBankPresence",
        "TurretStatePresence",
        "WeaponManifestPresence",
        "WeaponStatePresence",
    )
    for registry_name in presence_registries:
        enum_name = registry_name + "Flag"
        bindings[registry_name] = {
            "enum": enum_name,
            "prefix": enum_name,
            "known": f"Known{registry_name}Flags",
            "reserved": f"Reserved{registry_name}Flags",
        }
    return bindings


def transaction_constant(constants: dict[str, object], name: str) -> int:
    for item in constants["transactions"]:
        if item["name"] == name and isinstance(item.get("numeric_value"), int):
            return int(item["numeric_value"])
    raise SchemaError(f"normative transaction constant {name} is missing")


def verify_cpp_correspondence(
    registries: dict[str, dict[str, object]], constants: dict[str, object]
) -> dict[str, object]:
    cpp_enums, cpp_constants, cpp_bool_constants, cpp_typed_aliases = parse_cpp_protocol_constants()
    bindings = cpp_registry_bindings()
    if not set(bindings).issubset(registries):
        missing = sorted(set(bindings) - set(registries))
        raise SchemaError(f"C++ registry binding coverage mismatch; missing={missing}")
    bound_enums = {str(binding["enum"]) for binding in bindings.values()}
    implementation_only_enums = {"ProtocolMinorNegotiationResult"}
    comparable_cpp_enums = set(cpp_enums) - implementation_only_enums
    if comparable_cpp_enums != bound_enums:
        missing = sorted(comparable_cpp_enums - bound_enums)
        stale = sorted(bound_enums - set(cpp_enums))
        raise SchemaError(f"C++ enum coverage mismatch; unbound={missing}, missing_in_header={stale}")

    verified_constant_names: set[str] = set()
    for registry_name, registry in registries.items():
        if registry_name not in bindings:
            registry["cpp"] = {"status": "unbound"}
            continue
        binding = bindings[registry_name]
        cpp_name = str(binding["enum"])
        cpp_registry = cpp_enums[cpp_name]
        if int(cpp_registry["width_bits"]) != int(registry["width_bits"]):
            raise SchemaError(
                f"{registry_name} width drifts from C++ {cpp_name}: "
                f"u{registry['width_bits']} vs u{cpp_registry['width_bits']}"
            )
        prefix = str(binding.get("prefix", ""))
        aliases = {
            normalize_symbol(str(left)): normalize_symbol(str(right))
            for left, right in dict(binding.get("aliases", {})).items()
        }
        cpp_values: dict[str, int] = {}
        for item in cpp_registry["values"]:
            member_name = str(item["name"])
            if prefix and member_name.startswith(prefix):
                member_name = member_name[len(prefix):]
            normalized = normalize_symbol(member_name)
            if normalized == "none" and int(item["value"]) == 0 and registry["kind"] == "bitmask":
                continue
            normalized = aliases.get(normalized, normalized)
            if normalized in cpp_values:
                raise SchemaError(f"C++ enum {cpp_name} has colliding normalized member names")
            cpp_values[normalized] = int(item["value"])
        doc_values = {
            normalize_symbol(str(item["name"])): int(item["value"])
            for item in registry["values"]
        }
        if cpp_values != doc_values:
            raise SchemaError(f"numeric drift between {registry_name} and C++ {cpp_name}: {cpp_values} vs {doc_values}")

        cpp_metadata: dict[str, object] = {"enum": cpp_name}
        if prefix:
            cpp_metadata["member_prefix"] = prefix
        if binding.get("aliases"):
            cpp_metadata["member_aliases"] = dict(binding["aliases"])
        if registry_name == "Capability":
            expected_alias = {"type": "Capability", "target": "CapabilityUpdate"}
            if cpp_typed_aliases.get("CapabilityDynamicUpdate") != expected_alias:
                raise SchemaError("legacy CapabilityDynamicUpdate alias is missing or targets the wrong value")
            cpp_metadata["symbol_aliases"] = {"CapabilityDynamicUpdate": "CapabilityUpdate"}
        if "known" in binding or "reserved" in binding:
            known_name = str(binding["known"])
            reserved_name = str(binding["reserved"])
            if known_name not in cpp_constants or reserved_name not in cpp_constants:
                raise SchemaError(f"C++ masks for {registry_name} are missing")
            expected_known = int(registry["reserved"]["known_mask"])
            expected_reserved = int(registry["reserved"]["reserved_mask"])
            if cpp_constants[known_name] != expected_known or cpp_constants[reserved_name] != expected_reserved:
                raise SchemaError(
                    f"C++ known/reserved mask drift for {registry_name}: "
                    f"{cpp_constants[known_name]:#x}/{cpp_constants[reserved_name]:#x} vs "
                    f"{expected_known:#x}/{expected_reserved:#x}"
                )
            cpp_metadata["known_mask_constant"] = known_name
            cpp_metadata["reserved_mask_constant"] = reserved_name
            verified_constant_names.update((known_name, reserved_name))
        registry["cpp"] = cpp_metadata

    first_reserved_checks = {
        "FirstReservedMessageType": 21,
        "FirstReservedRecordType": 30,
        "FirstReservedCapabilityBit": 5,
        "FirstReservedCapabilityExtensionType": 3,
    }
    for name, expected in first_reserved_checks.items():
        if cpp_constants.get(name) != expected:
            raise SchemaError(f"C++ {name} drift: {cpp_constants.get(name)!r} vs {expected}")
        verified_constant_names.add(name)

    overlay_required = int(registries["OverlayCapabilityBit"]["required_mask"])
    if cpp_constants.get("RequiredOverlayCapabilityBits") != overlay_required:
        raise SchemaError("C++ RequiredOverlayCapabilityBits drifts from document 05")
    verified_constant_names.add("RequiredOverlayCapabilityBits")

    partial_value = next(item for item in registries["RecordFlags"]["values"] if item["name"] == "PARTIAL")
    target_video_extension = next(
        item
        for item in registries["CapabilityExtensionType"]["values"]
        if item["name"] == "TARGET_VIDEO_NEGOTIATION"
    )
    expected_bool_constants = {
        "RecordFlagPartialAllowedV1": bool(partial_value["emittable_v1"]),
        "TargetVideoNegotiationReservedNonEmittable": not bool(target_video_extension["emittable_v1"]),
    }
    if cpp_bool_constants != expected_bool_constants:
        raise SchemaError(
            f"C++ normative status constants drift: {cpp_bool_constants} vs {expected_bool_constants}"
        )
    if set(cpp_typed_aliases) != {"CapabilityDynamicUpdate"}:
        raise SchemaError(f"unverified C++ typed aliases: {sorted(cpp_typed_aliases)}")

    core = constants["core"]
    reassembly = constants["reassembly"]
    constant_bindings = {
        "Magic": int(core["magic_u32"]),
        "VersionMajor": int(core["version_major"]),
        "VersionMinor": int(core["version_minor"]),
        "HeaderSizeV1": int(core["header_size"]),
        "MaxDatagramSize": int(core["max_datagram_size"]),
        "MaxFragmentPayload": int(core["max_fragment_payload"]),
        "MaxStateMessageSize": int(reassembly[0]["max_message_size"]),
        "MaxVideoMessageSize": int(reassembly[1]["max_message_size"]),
        "MaxStateFragments": int(reassembly[0]["max_fragments"]),
        "MaxVideoFragments": int(reassembly[1]["max_fragments"]),
        "MaxStateReassembliesPerClient": int(reassembly[0]["max_concurrent_per_client"]),
        "MaxVideoReassembliesPerClient": int(reassembly[1]["max_concurrent_per_client"]),
        "MaxStateReassemblyBytesPerClient": int(reassembly[0]["reserved_bytes_per_client"]),
        "MaxVideoReassemblyBytesPerClient": int(reassembly[1]["reserved_bytes_per_client"]),
        "MaxStatePartSize": transaction_constant(constants, "TELEMETRY_MAX_STATE_PART_SIZE"),
        "MaxTransactionSize": transaction_constant(constants, "TELEMETRY_MAX_TRANSACTION_SIZE"),
        "MaxTransactionParts": transaction_constant(constants, "TELEMETRY_MAX_TRANSACTION_PARTS"),
        "MaxCandidateTransactionsPerClient": transaction_constant(
            constants, "TELEMETRY_MAX_CANDIDATE_TRANSACTIONS_PER_CLIENT"
        ),
        "MaxCandidateTransactionBytesPerClient": transaction_constant(
            constants, "TELEMETRY_MAX_CANDIDATE_TRANSACTION_BYTES_PER_CLIENT"
        ),
        "TransactionAssemblyTimeoutMs": transaction_constant(
            constants, "TELEMETRY_TRANSACTION_ASSEMBLY_TIMEOUT_MS"
        ),
    }
    for name, expected in constant_bindings.items():
        if cpp_constants.get(name) != expected:
            raise SchemaError(f"C++ constant {name} drifts: {cpp_constants.get(name)!r} vs {expected}")
        verified_constant_names.add(name)

    amendment_constants = {
        "VersionMinorV1_0": 0,
        "VersionMinorV1_1": 1,
        "LatestSupportedVersionMinor": 1,
        "KnownStateDomainCoverageBitsV1_0": 0x3FF,
        "KnownStateDomainCoverageBits": 0x3FF,
        "ReservedStateDomainCoverageBits": 0xFFFFFFFFFFFFFC00,
    }
    for name, expected in amendment_constants.items():
        if cpp_constants.get(name) != expected:
            raise SchemaError(f"C++ amendment constant {name} drifts: {cpp_constants.get(name)!r} vs {expected}")
        verified_constant_names.add(name)

    unverified_constants = sorted(set(cpp_constants) - verified_constant_names)
    if unverified_constants:
        raise SchemaError(f"C++ numeric constants lack schema correspondence: {unverified_constants}")
    return {
        "header": "code/telemetry/protocol/telemetry_protocol_constants.h",
        "document_registry_count": len(registries),
        "bound_registry_count": len(bindings),
        "unbound_registries": sorted(set(registries) - set(bindings)),
        "verified_enum_count": len(cpp_enums),
        "verified_constant_count": len(verified_constant_names),
        "verified_bool_constant_count": len(expected_bool_constants),
        "verified_symbol_alias_count": len(cpp_typed_aliases),
        "constant_bindings": [
            {"cpp_symbol": name, "value": value} for name, value in sorted(constant_bindings.items())
        ],
    }


def source_metadata(sources: dict[str, str]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for name in SOURCE_NAMES:
        encoded = sources[name].encode("utf-8")
        result.append(
            {
                "path": f"documentation/analysis/specs/0-Contrat-de-protocole/{name}",
                "sha256": hashlib.sha256(encoded).hexdigest(),
            }
        )
    return result


def merge_numeric_registries(
    target: dict[str, dict[str, object]], additions: dict[str, dict[str, object]]
) -> None:
    for name, registry in additions.items():
        if name not in target:
            target[name] = registry
            continue
        existing = target[name]
        if existing["kind"] != registry["kind"] or existing["width_bits"] != registry["width_bits"]:
            raise SchemaError(f"duplicate numeric registry {name} changes kind or width")
        assert_same_registry_values(name, existing["values"], registry["values"], "duplicate registry")
        additional_sources = list(existing.get("additional_sources", ()))
        additional_sources.append(registry["source"])
        existing["additional_sources"] = additional_sources


def build_schema() -> dict[str, object]:
    sources = read_sources()
    doc01 = sources["01-cadre-normatif-et-perimetre.md"]
    doc02 = sources["02-format-filaire-et-registres.md"]
    doc03 = sources["03-session-horloges-fiabilite.md"]
    doc04 = sources["04-modele-de-donnees-v1.md"]
    doc05 = sources["05-capabilities-et-vues-specialisees.md"]
    doc06 = sources["06-validation-securite-et-conformite.md"]

    invalid_message, messages = parse_message_types(doc02, doc03, doc05)
    message_layouts = parse_message_layouts(doc02, doc03, doc05, messages)
    messages_with_layouts = [
        {**message, **message_layouts[int(message["id"])]} for message in messages
    ]
    invalid_record, records = parse_record_registry(doc02, doc04, doc05)
    layouts = parse_record_layouts(doc04, records)
    structured_types = parse_record_structured_types(doc04)
    structured_type_names = {str(item["name"]) for item in structured_types}
    structured_by_name = {str(item["name"]): item for item in structured_types}
    constants = parse_constants(doc02, doc03, doc05, doc06)
    core_constants = constants["core"]
    records_with_layouts: list[dict[str, object]] = []
    for record in records:
        record_id = int(record["id"])
        layout = layouts[record_id]
        direct_nested_types = sorted(
            {
                reference
                for field in layout["fields"]
                for reference in structured_type_references(str(field["wire"]))
            }
        )
        unknown_nested = sorted(set(direct_nested_types) - structured_type_names)
        if unknown_nested:
            raise SchemaError(f"RecordType {record_id} references unknown nested layouts: {unknown_nested}")
        nested_closure = set(direct_nested_types)
        pending_nested = list(direct_nested_types)
        while pending_nested:
            nested_name = pending_nested.pop()
            for reference in structured_by_name[nested_name]["nested_types"]:
                if reference not in nested_closure:
                    nested_closure.add(reference)
                    pending_nested.append(reference)
        enriched_fields: list[dict[str, object]] = []
        for original_field in layout["fields"]:
            field = dict(original_field)
            if str(field["name"]) != "presence":
                bits = sorted(
                    {
                        int(value)
                        for value in re.findall(r"\bbit\s*([0-9]+)\b", str(field.get("semantics", "")))
                    }
                )
                if bits:
                    field["presence_condition"] = {"selector": "presence", "bits": bits}
            enriched_fields.append(field)
        records_with_layouts.append(
            {
                **record,
                **layout,
                "fields": enriched_fields,
                "direct_nested_types": direct_nested_types,
                "nested_types": sorted(nested_closure),
            }
        )

    capabilities = parse_capabilities(doc02, doc05)
    validation_errors = parse_validation_errors(doc06)
    numeric_registries: dict[str, dict[str, object]] = {
        "MessageType": make_enum_registry(
            "MessageType",
            8,
            [
                {
                    "name": upper_snake(str(item["name"])),
                    "value": int(item["id"]),
                    "direction": item["direction"],
                    "payload": item["payload"],
                    **({"specialized": item["specialized"]} if "specialized" in item else {}),
                }
                for item in (invalid_message, *messages)
            ],
            registry_source("02-format-filaire-et-registres.md", "6"),
            unknown_policy="ignore",
            unknown_handling="drop after header validation; optionally emit a rate-limited NACK",
            reserved_ranges=((21, 255),),
        ),
        "RecordType": make_enum_registry(
            "RecordType",
            16,
            [
                {
                    "name": upper_snake(str(item["name"])),
                    "value": int(item["id"]),
                    **({"scope": item["scope"]} if "scope" in item else {}),
                    **({"containers": item["containers"]} if "containers" in item else {}),
                }
                for item in (invalid_record, *records)
            ],
            registry_source("02-format-filaire-et-registres.md", "7.1"),
            unknown_policy="ignore",
            unknown_handling="skip by record_length; semantic validation fails when the type is required",
            reserved_ranges=((29, 65535),),
        ),
        "ValidationError": make_enum_registry(
            "ValidationError",
            8,
            [
                {"name": upper_snake(str(item["name"])), "value": int(item["id"]), "level": item["level"]}
                for item in validation_errors
            ],
            registry_source("06-validation-securite-et-conformite.md", "7"),
            reserved_ranges=((48, 255),),
        ),
    }
    merge_numeric_registries(numeric_registries, parse_doc02_numeric_registries(doc02, capabilities))
    merge_numeric_registries(numeric_registries, parse_doc04_numeric_registries(doc04))
    merge_numeric_registries(numeric_registries, parse_doc05_numeric_registries(doc02, doc05, numeric_registries))
    cpp_correspondence = verify_cpp_correspondence(numeric_registries, constants)
    presence_registries = [name for name in numeric_registries if name.endswith("Presence")]
    numeric_registry_summary = {
        "registry_count": len(numeric_registries),
        "presence_registry_count": len(presence_registries),
        "presence_assigned_bit_count": sum(
            len(numeric_registries[name]["values"]) for name in presence_registries
        ),
        "cpp_bound_registry_count": cpp_correspondence["bound_registry_count"],
    }

    schema: dict[str, object] = {
        "schema_format": "FSTL-machine-readable-registry-v1",
        "protocol": "FSTL",
        "wire_version": f"{core_constants['version_major']}.{core_constants['version_minor']}",
        "wire_version_scope": "frozen FSTL 1.0 compatibility view",
        "supported_wire_versions": {
            "1.0": {"minor": 0, "state_domain_known_mask": 0x3FF,
                    "player_kinematics": "reserved_and_rejected"},
            "1.1": {"minor": 1, "state_domain_known_mask": 0x7FF,
                    "player_kinematics": 0x400},
        },
        "producer_profiles": {
            "phase1_minimal": {"minimum_minor": 1, "maximum_minor": 1,
                               "required_state_domain_coverage": 0x400}
        },
        "phase1_player_kinematics_record_set": {
            "required_always": ["SESSION_STATE", "MISSION_STATE"],
            "required_when_observed_player_present": ["ENTITY_LIFECYCLE", "FLIGHT_STATE"],
            "exact_record_set": True,
            "same_observed_player_entity_id": True,
            "authority_mode": "SOLO",
            "visibility_mode": "COCKPIT",
            "entity_lifecycle_object_type": "SHIP",
            "entity_lifecycle_presence": 0,
            "flight_state_presence": 0,
            "required_manifest_id": 0,
            "negotiated_capabilities": 0,
            "event_coverage_state_derived": 0,
            "event_coverage_exact": 0,
            "forbidden_records": ["SHIP_IDENTITY"],
            "promotion": {"requires_core_ship_coverage": True,
                          "requires_nonzero_manifest_id": True,
                          "requires_complete_core_ship_record_set": True},
        },
        "generated_by": "test/telemetry/protocol/tools/fstl_schema.py",
        "normative_sources": source_metadata(sources),
        "scalar_types": parse_scalar_types(doc02),
        "wire_conventions": parse_wire_conventions(doc01, doc02, doc03),
        "constants": constants,
        "datagram_header": {
            "size": core_constants["header_size"],
            "fields": parse_datagram_header(doc02),
        },
        "message_type_invalid": invalid_message,
        "message_types": messages_with_layouts,
        "record_type_invalid": invalid_record,
        "record_types": records_with_layouts,
        "record_structured_types": structured_types,
        "wire_aliases": parse_wire_aliases(doc04),
        "capabilities": capabilities,
        "validation_errors": validation_errors,
        "numeric_registries": numeric_registries,
        "numeric_registry_summary": numeric_registry_summary,
        "cpp_correspondence": cpp_correspondence,
        "unknown_value_policy": parse_unknown_value_policy(doc06),
    }
    validate_schema_shape(schema)
    return schema


def validate_schema_shape(schema: dict[str, object]) -> None:
    expected_versions = {
        "1.0": {"minor": 0, "state_domain_known_mask": 0x3FF,
                "player_kinematics": "reserved_and_rejected"},
        "1.1": {"minor": 1, "state_domain_known_mask": 0x7FF,
                "player_kinematics": 0x400},
    }
    expected_profiles = {"phase1_minimal": {"minimum_minor": 1, "maximum_minor": 1,
                                             "required_state_domain_coverage": 0x400}}
    if schema.get("supported_wire_versions") != expected_versions:
        raise SchemaError("supported FSTL 1.0/1.1 wire-version contract drift")
    if schema.get("producer_profiles") != expected_profiles:
        raise SchemaError("Phase 1 producer profile drift")
    messages = schema.get("message_types")
    records = schema.get("record_types")
    structured_types = schema.get("record_structured_types")
    wire_aliases = schema.get("wire_aliases")
    capabilities = schema.get("capabilities")
    errors = schema.get("validation_errors")
    numeric_registries = schema.get("numeric_registries")
    wire_conventions = schema.get("wire_conventions")
    phase1 = schema.get("phase1_player_kinematics_record_set")
    expected_phase1 = {
        "required_always": ["SESSION_STATE", "MISSION_STATE"],
        "required_when_observed_player_present": ["ENTITY_LIFECYCLE", "FLIGHT_STATE"],
        "exact_record_set": True, "same_observed_player_entity_id": True,
        "authority_mode": "SOLO", "visibility_mode": "COCKPIT",
        "entity_lifecycle_object_type": "SHIP", "entity_lifecycle_presence": 0,
        "flight_state_presence": 0, "required_manifest_id": 0,
        "negotiated_capabilities": 0, "event_coverage_state_derived": 0,
        "event_coverage_exact": 0, "forbidden_records": ["SHIP_IDENTITY"],
        "promotion": {"requires_core_ship_coverage": True,
                      "requires_nonzero_manifest_id": True,
                      "requires_complete_core_ship_record_set": True},
    }
    if not isinstance(phase1, dict):
        raise SchemaError("missing Phase 1 PLAYER_KINEMATICS record-set contract")
    if phase1 != expected_phase1:
        raise SchemaError("Phase 1 PLAYER_KINEMATICS record-set invariant drift")
    if not isinstance(messages, list) or not isinstance(records, list):
        raise SchemaError("schema registries must be arrays")
    if not isinstance(structured_types, list) or not structured_types:
        raise SchemaError("schema record_structured_types must be a non-empty array")
    if not isinstance(wire_aliases, list) or not wire_aliases:
        raise SchemaError("schema wire_aliases must be a non-empty array")
    if not isinstance(capabilities, list) or not isinstance(errors, list):
        raise SchemaError("schema capability/error registries must be arrays")
    if not isinstance(numeric_registries, dict) or not numeric_registries:
        raise SchemaError("schema numeric_registries must be a non-empty object")
    if not isinstance(wire_conventions, dict):
        raise SchemaError("schema wire_conventions must be an object")
    require_contiguous(messages, "id", 1, 20, "schema MessageType")
    record_type_maximum = 29 if schema.get("wire_version") == "1.1" else 28
    require_contiguous(records, "id", 1, record_type_maximum, "schema RecordType")
    require_contiguous(capabilities, "bit", 0, 4, "schema Capability")
    require_contiguous(errors, "id", 0, 47, "schema ValidationError")
    for registry_name, entries in (
        ("MessageType", messages),
        ("RecordType", records),
        ("Capability", capabilities),
        ("ValidationError", errors),
    ):
        require_unique(entries, "name", f"schema {registry_name}")
        require_unique_normalized_names(entries, "name", f"schema {registry_name}")
    for message in messages:
        fields = message.get("fields")
        qos = message.get("qos")
        limits = message.get("limits")
        if not isinstance(fields, list) or not fields:
            raise SchemaError(f"schema MessageType {message.get('id')} has no payload fields")
        if not isinstance(qos, list) or not qos:
            raise SchemaError(f"schema MessageType {message.get('id')} has no QoS variants")
        if not isinstance(limits, dict) or "fixed_prefix_bytes" not in limits:
            raise SchemaError(f"schema MessageType {message.get('id')} has no payload limits")
        require_unique(fields, "name", f"schema MessageType {message.get('id')} fields")
        require_unique_normalized_names(fields, "name", f"schema MessageType {message.get('id')} fields")
        offsets = [field.get("offset") for field in fields]
        if not all(isinstance(value, int) for value in offsets):
            raise SchemaError(f"schema MessageType {message.get('id')} has non-numeric offsets")
        if offsets != sorted(offsets) or len(offsets) != len(set(offsets)):
            raise SchemaError(f"schema MessageType {message.get('id')} offsets collide or regress")
    for record in records:
        fields = record.get("fields")
        if not isinstance(fields, list) or not fields:
            raise SchemaError(f"schema RecordType {record.get('id')} has no top-level fields")
        require_unique(fields, "name", f"schema RecordType {record.get('id')} fields")
        require_unique_normalized_names(fields, "name", f"schema RecordType {record.get('id')} fields")

    require_unique(structured_types, "name", "schema nested record structures")
    structured_by_name = {str(item["name"]): item for item in structured_types}
    if "EventItemV1" not in structured_by_name:
        raise SchemaError("schema must expose the EventItemV1 nested layout")
    for structured in structured_types:
        fields = structured.get("fields")
        if not isinstance(fields, list) or not fields:
            raise SchemaError(f"schema nested layout {structured.get('name')} has no fields")
        for field in fields:
            if not isinstance(field.get("name"), str) or not isinstance(field.get("wire"), str):
                raise SchemaError(f"schema nested layout {structured.get('name')} has an invalid field")
    referenced_nested: set[str] = set()
    for record in records:
        for field in record["fields"]:
            referenced_nested.update(structured_type_references(str(field["wire"])))
    pending = list(referenced_nested)
    while pending:
        name = pending.pop()
        if name not in structured_by_name:
            raise SchemaError(f"schema references unresolved nested layout {name}")
        for field in structured_by_name[name]["fields"]:
            for reference in structured_type_references(str(field["wire"])):
                if reference not in referenced_nested:
                    referenced_nested.add(reference)
                    pending.append(reference)
    if referenced_nested != set(structured_by_name):
        raise SchemaError(
            "schema nested layout reachability drift; "
            f"unreferenced={sorted(set(structured_by_name) - referenced_nested)}"
        )
    require_unique(wire_aliases, "name", "schema wire aliases")

    required_wire_conventions = {
        "bool8",
        "opaque_bytes",
        "string",
        "list",
        "vlist",
        "absence",
        "float32_canonicalization",
        "units_and_frame",
        "quaternion",
        "clock_domains",
        "limit_classification",
    }
    if set(wire_conventions) != required_wire_conventions:
        raise SchemaError(
            f"wire convention coverage drift; expected {sorted(required_wire_conventions)}, "
            f"got {sorted(wire_conventions)}"
        )
    for registry_name, registry_value in numeric_registries.items():
        if not isinstance(registry_value, dict):
            raise SchemaError(f"numeric registry {registry_name} must be an object")
        kind = registry_value.get("kind")
        width_bits = registry_value.get("width_bits")
        values = registry_value.get("values")
        reserved = registry_value.get("reserved")
        if kind not in ("enum", "bitmask") or width_bits not in (8, 16, 32, 64):
            raise SchemaError(f"numeric registry {registry_name} has invalid kind/width")
        if registry_value.get("unknown_policy") not in ("ignore", "reject"):
            raise SchemaError(f"numeric registry {registry_name} must declare unknown_policy ignore or reject")
        if not isinstance(values, list) or not isinstance(reserved, dict):
            raise SchemaError(f"numeric registry {registry_name} has invalid values/reserved policy")
        if kind == "enum" and not values:
            raise SchemaError(f"numeric enum {registry_name} must contain at least one value")
        require_unique(values, "name", f"schema numeric registry {registry_name}")
        require_unique_normalized_names(values, "name", f"schema numeric registry {registry_name}")
        require_unique(values, "value", f"schema numeric registry {registry_name}")
        maximum = (1 << int(width_bits)) - 1
        for item in values:
            value = int(item["value"])
            if value < 0 or value > maximum:
                raise SchemaError(f"schema numeric registry {registry_name} value is out of width")
        if kind == "bitmask":
            known = 0
            for item in values:
                value = int(item["value"])
                if value <= 0 or value & (value - 1):
                    raise SchemaError(f"schema numeric registry {registry_name} contains a non-bit value")
                if int(item.get("bit", -1)) != value.bit_length() - 1:
                    raise SchemaError(f"schema numeric registry {registry_name} bit/value drift")
                known |= value
            if reserved.get("known_mask") != known or reserved.get("reserved_mask") != maximum ^ known:
                raise SchemaError(f"schema numeric registry {registry_name} known/reserved mask drift")
        cpp = registry_value.get("cpp")
        if not isinstance(cpp, dict) or (not cpp.get("enum") and cpp.get("status") != "unbound"):
            raise SchemaError(f"numeric registry {registry_name} has no verified C++ binding")


def render_schema(schema: dict[str, object]) -> str:
    return json.dumps(schema, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def write_schema(expected: str, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(expected)


def check_schema(expected: str, path: Path) -> None:
    try:
        actual = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise SchemaError(f"schema is missing: {path}; run this tool with --write") from exc
    except (OSError, UnicodeError) as exc:
        raise SchemaError(f"cannot read schema {path}: {exc}") from exc

    try:
        loaded = json.loads(actual)
    except json.JSONDecodeError as exc:
        raise SchemaError(f"schema is not valid JSON/YAML 1.2 subset: {exc}") from exc
    if not isinstance(loaded, dict):
        raise SchemaError("schema root must be an object")
    validate_schema_shape(loaded)

    if actual != expected:
        diff = list(
            difflib.unified_diff(
                actual.splitlines(),
                expected.splitlines(),
                fromfile=str(path),
                tofile="regenerated from normative Markdown",
                lineterm="",
            )
        )
        preview = "\n".join(diff[:200])
        if len(diff) > 200:
            preview += f"\n... diff truncated ({len(diff) - 200} more lines)"
        raise SchemaError(f"machine-readable schema drift detected:\n{preview}")


def run_negative_self_tests(schema: dict[str, object]) -> tuple[str, ...]:
    passed: list[str] = []

    def clone(value: object) -> object:
        return json.loads(json.dumps(value, ensure_ascii=False))

    def expect_failure(label: str, operation: object) -> None:
        try:
            operation()  # type: ignore[operator]
        except SchemaError:
            passed.append(label)
            return
        raise SchemaError(f"negative self-test unexpectedly passed: {label}")

    collision_schema = clone(schema)
    assert isinstance(collision_schema, dict)
    collision_values = collision_schema["numeric_registries"]["MessageType"]["values"]
    collision_values[1]["value"] = collision_values[0]["value"]
    expect_failure("registry-collision", lambda: validate_schema_shape(collision_schema))

    reassigned_registries = clone(schema["numeric_registries"])
    assert isinstance(reassigned_registries, dict)
    reassigned_registries["RecordType"]["values"][1]["value"] = 500
    expect_failure(
        "cpp-reassignment-drift",
        lambda: verify_cpp_correspondence(reassigned_registries, schema["constants"]),
    )

    reserved_registries = clone(schema["numeric_registries"])
    assert isinstance(reserved_registries, dict)
    reserved_registries["MessageFlags"]["reserved"]["known_mask"] ^= 0x20
    expect_failure(
        "reserved-mask-drift",
        lambda: verify_cpp_correspondence(reserved_registries, schema["constants"]),
    )

    drifted_schema = clone(schema)
    assert isinstance(drifted_schema, dict)
    drifted_schema["wire_version"] = "1.255"
    expected = render_schema(schema)
    with tempfile.TemporaryDirectory(prefix="fstl-schema-negative-") as directory:
        path = Path(directory) / "fstl-v1.yaml"
        write_schema(render_schema(drifted_schema), path)
        expect_failure("checked-artifact-drift", lambda: check_schema(expected, path))
    phase1_contract = schema["phase1_player_kinematics_record_set"]
    assert isinstance(phase1_contract, dict)
    for field in phase1_contract:
        if field == "promotion":
            for promotion_field in phase1_contract[field]:
                mutated = clone(schema)
                mutated["phase1_player_kinematics_record_set"][field][promotion_field] = False
                expect_failure(f"phase1-{promotion_field}-drift", lambda value=mutated: validate_schema_shape(value))
        else:
            mutated = clone(schema)
            mutated["phase1_player_kinematics_record_set"][field] = None
            expect_failure(f"phase1-{field}-drift", lambda value=mutated: validate_schema_shape(value))
    for label, section in (("supported-wire-versions", "supported_wire_versions"),
                           ("producer-profiles", "producer_profiles")):
        mutated = clone(schema)
        mutated[section] = {}
        expect_failure(f"{label}-drift", lambda value=mutated: validate_schema_shape(value))
    return tuple(passed)


def run_schema_vector_self_test(schema_path: Path) -> str:
    process = subprocess.run(
        [sys.executable, "-B", str(SCHEMA_VECTOR_CHECKER_PATH), "--check", "--schema", str(schema_path)],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        raise SchemaError(f"schema/golden byte self-test failed: {detail}")
    return process.stdout.strip()


def phase1_provenance_path(index: int) -> str:
    return (PHASE1_DIR / PHASE1_SOURCE_NAMES[index]).relative_to(REPO_ROOT).as_posix()


def load_frozen_v1_0_schema() -> dict[str, object]:
    try:
        data = SCHEMA_V1_0_PATH.read_bytes()
    except OSError as exc:
        raise SchemaError(f"cannot read frozen FSTL 1.0 schema {SCHEMA_V1_0_PATH}: {exc}") from exc
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != FROZEN_SCHEMA_V1_0_BYTES or digest != FROZEN_SCHEMA_V1_0_SHA256:
        raise SchemaError(
            f"frozen FSTL 1.0 schema drift: bytes={len(data)}, sha256={digest}"
        )
    try:
        schema = json.loads(data)
    except json.JSONDecodeError as exc:
        raise SchemaError(f"frozen FSTL 1.0 schema is invalid JSON: {exc}") from exc
    if schema.get("wire_version") != "1.0":
        raise SchemaError("frozen FSTL 1.0 schema identity drift")
    return schema


def load_frozen_v1_0_ledger() -> tuple[dict[str, object], str, int]:
    try:
        data = FROZEN_LEDGER_PATH.read_bytes()
        ledger = json.loads(data)
    except (OSError, json.JSONDecodeError) as exc:
        raise SchemaError(f"cannot read FSTL 1.0 frozen-artifact ledger: {exc}") from exc
    if (ledger.get("schema") != "FSTL-1.0-FROZEN-ARTIFACTS" or
            ledger.get("wireVersion") != "1.0" or
            ledger.get("fileCount") != FROZEN_ARTIFACT_COUNT_V1_0 or
            ledger.get("treeSha256") != FROZEN_ARTIFACT_TREE_SHA256_V1_0):
        raise SchemaError("FSTL 1.0 frozen-artifact ledger identity drift")
    return ledger, hashlib.sha256(data).hexdigest(), len(data)


def field_by_name(fields: object, name: str) -> dict[str, object]:
    if not isinstance(fields, list):
        raise SchemaError(f"schema field collection is not a list while looking for {name}")
    matches = [field for field in fields if isinstance(field, dict) and field.get("name") == name]
    if len(matches) != 1:
        raise SchemaError(f"schema field {name} has {len(matches)} matches")
    return matches[0]


def registry_value_by_name(registry: dict[str, object], name: str) -> dict[str, object]:
    values = registry.get("values")
    if not isinstance(values, list):
        raise SchemaError(f"registry values are missing while looking for {name}")
    matches = [value for value in values if isinstance(value, dict) and value.get("name") == name]
    if len(matches) != 1:
        raise SchemaError(f"registry value {name} has {len(matches)} matches")
    return matches[0]


def build_fstl_v1_1_schema() -> dict[str, object]:
    """Derive the additive 1.1 view from the immutable 1.0 schema."""

    schema = json.loads(json.dumps(load_frozen_v1_0_schema()))
    assert isinstance(schema, dict)
    _, ledger_sha256, ledger_bytes = load_frozen_v1_0_ledger()

    schema["wire_version"] = "1.1"
    schema["wire_version_scope"] = "additive FSTL 1.1 amendment view"
    schema.pop("normative_sources", None)
    schema["base_schema"] = {
        "path": SCHEMA_V1_0_PATH.relative_to(REPO_ROOT).as_posix(),
        "wire_version": "1.0",
        "bytes": FROZEN_SCHEMA_V1_0_BYTES,
        "sha256": FROZEN_SCHEMA_V1_0_SHA256,
    }
    schema["base_artifact_manifest"] = {
        "path": FROZEN_LEDGER_PATH.relative_to(REPO_ROOT).as_posix(),
        "schema": "FSTL-1.0-FROZEN-ARTIFACTS",
        "wire_version": "1.0",
        "bytes": ledger_bytes,
        "sha256": ledger_sha256,
        "file_count": FROZEN_ARTIFACT_COUNT_V1_0,
        "tree_sha256": FROZEN_ARTIFACT_TREE_SHA256_V1_0,
    }
    p1_doc01_path = phase1_provenance_path(0)
    p1_doc04_path = phase1_provenance_path(3)
    p1_doc07_path = phase1_provenance_path(6)
    p3_doc04_path = (
        "documentation/analysis/specs/3-Ciblage-et-capteurs/"
        "04-modele-de-donnees-et-regles-metier.md"
    )
    schema["amendment_provenance"] = {
        "normative": False,
        "player_kinematics": [
            {"document": p1_doc01_path, "requirements": ["P1-REQ-020"]},
            {"document": p1_doc04_path, "requirements": ["P1-REQ-020"]},
            {"document": p1_doc07_path, "requirements": ["P1-REQ-020"]},
        ],
        "phase1_minimal_profile": [
            {"document": p1_doc01_path,
             "requirements": ["P1-REQ-021", "P1-REQ-022", "P1-REQ-023"]},
            {"document": p1_doc04_path,
             "requirements": ["P1-REQ-021", "P1-REQ-022", "P1-REQ-023"]},
            {"document": p1_doc07_path,
             "requirements": ["P1-REQ-021", "P1-REQ-022", "P1-REQ-023"]},
        ],
        "phase3_radar_projection": [
            {"document": p3_doc04_path,
             "requirements": ["P3-REQ-024", "P3-REQ-025", "P3-REQ-033"]},
        ],
        "phase3_hud_alerts": [
            {"document": p3_doc04_path,
             "requirements": ["P3-REQ-012", "P3-REQ-027", "P3-REQ-033", "P3-REQ-047"]},
        ],
    }

    schema["supported_wire_versions"] = {
        "1.0": {"minor": 0, "state_domain_known_mask": 0x3FF,
                "player_kinematics": "reserved_and_rejected"},
        "1.1": {"minor": 1, "state_domain_known_mask": 0x7FF,
                "player_kinematics": 0x400},
    }
    schema["producer_profiles"] = {
        "phase1_minimal": {"minimum_minor": 1, "maximum_minor": 1,
                           "required_state_domain_coverage": 0x400}
    }
    schema["phase1_player_kinematics_record_set"] = {
        "required_always": ["SESSION_STATE", "MISSION_STATE"],
        "required_when_observed_player_present": ["ENTITY_LIFECYCLE", "FLIGHT_STATE"],
        "exact_record_set": True,
        "same_observed_player_entity_id": True,
        "authority_mode": "SOLO",
        "visibility_mode": "COCKPIT",
        "entity_lifecycle_object_type": "SHIP",
        "entity_lifecycle_presence": 0,
        "flight_state_presence": 0,
        "required_manifest_id": 0,
        "negotiated_capabilities": 0,
        "event_coverage_state_derived": 0,
        "event_coverage_exact": 0,
        "forbidden_records": ["SHIP_IDENTITY"],
        "promotion": {"requires_core_ship_coverage": True,
                      "requires_nonzero_manifest_id": True,
                      "requires_complete_core_ship_record_set": True},
    }

    header = schema.get("datagram_header")
    if not isinstance(header, dict):
        raise SchemaError("base schema datagram header is missing")
    field_by_name(header.get("fields"), "version_minor")["rule"] = (
        "0 en FSTL 1.0 ; 1 en FSTL 1.1 accepté"
    )
    messages = schema.get("message_types")
    if not isinstance(messages, list):
        raise SchemaError("base schema message registry is missing")
    messages_by_id = {int(message["id"]): message for message in messages}
    field_by_name(messages_by_id[1]["fields"], "min_minor")["rule"] = "mineure minimale supportée, 0 ou 1"
    field_by_name(messages_by_id[1]["fields"], "max_minor")["rule"] = "mineure maximale supportée, min_minor..1"
    field_by_name(messages_by_id[2]["fields"], "min_minor")["rule"] = "mineure minimale supportée, 0 ou 1"
    field_by_name(messages_by_id[2]["fields"], "max_minor")["rule"] = "mineure maximale supportée, min_minor..1"
    field_by_name(messages_by_id[3]["fields"], "selected_minor")["rule"] = (
        "0 ou 1 si accepté, plus haute mineure commune"
    )

    registries = schema.get("numeric_registries")
    if not isinstance(registries, dict):
        raise SchemaError("base schema numeric registries are missing")
    record_type_registry = registries["RecordType"]
    record_type_registry["reserved"] = {"policy": "reject", "ranges": [[30, 65535]]}
    record_type_registry["values"].append({
        "name": "HUD_ALERT_STATE",
        "value": 29,
        "source": {"document": p3_doc04_path, "section": "10"},
    })

    registries["HudAlertStatePresence"] = {
        "kind": "bitmask",
        "width_bits": 64,
        "unknown_policy": "reject",
        "cpp": {
            "enum": "HudAlertStatePresenceFlag",
            "member_prefix": "HudAlertStatePresenceFlag",
            "known_mask_constant": "KnownHudAlertStatePresenceFlags",
            "reserved_mask_constant": "ReservedHudAlertStatePresenceFlags",
        },
        "source": {"document": p3_doc04_path, "section": "10"},
        "reserved": {"known_mask": 0x1, "reserved_mask": 0xFFFFFFFFFFFFFFFE},
        "values": [{"bit": 0, "name": "ACTIVE_WARNING", "value": 0x1}],
    }
    registries["HudAlertMissileLockState"] = {
        "kind": "enum",
        "width_bits": 8,
        "unknown_policy": "reject",
        "cpp": {"enum": "HudAlertMissileLockState"},
        "source": {"document": p3_doc04_path, "section": "10"},
        "reserved": {"policy": "reject", "ranges": [[3, 255]]},
        "values": [
            {"name": "NONE", "value": 0},
            {"name": "ATTEMPT", "value": 1},
            {"name": "ACQUIRED", "value": 2},
        ],
    }
    registries["HudAlertWarningKind"] = {
        "kind": "enum",
        "width_bits": 8,
        "unknown_policy": "reject",
        "cpp": {"enum": "HudAlertWarningKind"},
        "source": {"document": p3_doc04_path, "section": "10"},
        "reserved": {"policy": "reject", "ranges": [[0, 0], [8, 255]]},
        "values": [
            {"name": "LAUNCH", "value": 1},
            {"name": "EVADED", "value": 2},
            {"name": "COLLISION", "value": 3},
            {"name": "BLAST", "value": 4},
            {"name": "ENGINE_WASH", "value": 5},
            {"name": "EMP", "value": 6},
            {"name": "OTHER", "value": 7},
        ],
    }
    state_coverage = registries["StateDomainCoverage"]
    state_coverage["cpp"] = {
        "enum": "StateDomainCoverageBit",
        "member_prefix": "StateDomainCoverageBit",
        "known_mask_constant": "KnownStateDomainCoverageBitsV1_1",
        "reserved_mask_constant": "ReservedStateDomainCoverageBitsV1_1",
    }
    state_coverage["reserved"]["known_mask"] = 0x7FF
    state_coverage["reserved"]["reserved_mask"] = 0xFFFFFFFFFFFFF800
    values = state_coverage["values"]
    values.append({
        "bit": 10,
        "name": "PLAYER_KINEMATICS",
        "value": 0x400,
        "source": {"document": p1_doc04_path, "section": "2.1"},
    })
    values.sort(key=lambda item: int(item["value"]))

    radar_contact_presence = registries["RadarContactsPresence"]
    radar_contact_presence["reserved"]["known_mask"] = 0xFF
    radar_contact_presence["reserved"]["reserved_mask"] = 0xFFFFFFFFFFFFFF00
    radar_contact_presence["values"].append({
        "bit": 6,
        "name": "HUD_TYPE_LABEL",
        "value": 0x40,
        "source": {"document": p3_doc04_path, "section": "8.2"},
    })
    radar_contact_presence["values"].append({
        "bit": 7,
        "name": "RADAR_VISUAL",
        "value": 0x80,
        "source": {"document": p3_doc04_path, "section": "8.2"},
    })
    radar_contact_presence["values"].sort(key=lambda item: int(item["value"]))

    registries["RadarBlipType"] = {
        "kind": "enum",
        "width_bits": 8,
        "unknown_policy": "reject",
        "cpp": {"enum": "RadarBlipType"},
        "source": {"document": p3_doc04_path, "section": "8.2"},
        "reserved": {"policy": "reject", "ranges": [[6, 255]]},
        "values": [
            {"name": "JUMP_NODE", "value": 0},
            {"name": "NAVBUOY_CARGO", "value": 1},
            {"name": "BOMB", "value": 2},
            {"name": "WARPING_SHIP", "value": 3},
            {"name": "TAGGED_SHIP", "value": 4},
            {"name": "NORMAL_SHIP", "value": 5},
        ],
    }

    records = schema.get("record_types")
    if not isinstance(records, list):
        raise SchemaError("base schema record registry is missing")
    session_state = next(record for record in records if int(record["id"]) == 1)
    field_by_name(session_state["fields"], "state_domain_coverage")["semantics"] = (
        "domaines garantis complets par rapport au mode ; CORE_SHIP obligatoire en FSTL 1.0 ; "
        "PLAYER_KINEMATICS obligatoire en FSTL 1.1 et peut être le seul domaine du profil Phase 1"
    )
    radar_contacts = next(record for record in records if int(record["id"]) == 18)
    radar_v1_fields = json.loads(json.dumps(radar_contacts["fields"]))
    radar_v2_fields = json.loads(json.dumps(radar_v1_fields))
    velocity_index = next(
        index for index, field in enumerate(radar_v2_fields)
        if field["name"] == "velocity_world"
    )
    radar_v2_fields[velocity_index + 1:velocity_index + 1] = [
        {
            "constraint": "position locale bornée",
            "name": "radar_local_position",
            "nature": "A",
            "position": "10",
            "semantics": (
                "contact dans le repère du radar standard capturé au même "
                "tick que radar_project_contact"
            ),
            "wire": "vec3f",
        },
        {
            "constraint": "[0;1,0e12] wu",
            "name": "radar_projection_distance",
            "nature": "A",
            "position": "11",
            "semantics": "RadarContactProjection.distance du même tick",
            "wire": "float32",
        },
    ]
    for index, field in enumerate(radar_v2_fields, start=1):
        field["position"] = str(index)
    radar_v3_fields = json.loads(json.dumps(radar_v2_fields))
    radar_v3_fields.append({
        "constraint": "UTF-8 1..255 octets; vaisseau VISIBLE uniquement",
        "name": "hud_type_label",
        "nature": "A",
        "position": str(len(radar_v3_fields) + 1),
        "presence_condition": {"bits": [6], "selector": "presence"},
        "semantics": (
            "bit 6; exact second line rendered by the FSO Target Box; "
            "independent from CLASS_MANIFEST"
        ),
        "wire": "str<255>",
    })
    radar_v4_fields = json.loads(json.dumps(radar_v3_fields))
    radar_v4_fields.extend([
        {
            "constraint": "RGBA final rÃ©solu par FSO",
            "name": "radar_blip_color",
            "nature": "A",
            "position": str(len(radar_v4_fields) + 1),
            "presence_condition": {"bits": [7], "selector": "presence"},
            "semantics": "bit 7; couleur autoritaire du blip, alpha inclus",
            "wire": "rgba8",
        },
        {
            "constraint": "enum FSO fermÃ© 0..5",
            "name": "radar_blip_type",
            "nature": "A",
            "position": str(len(radar_v4_fields) + 2),
            "presence_condition": {"bits": [7], "selector": "presence"},
            "semantics": "bit 7; BLIP_TYPE_* autoritaire correspondant Ã  la couleur",
            "wire": "RadarBlipType",
        },
    ])
    radar_contacts["versions"] = [
        {"version": 1, "compatibility": "frozen FSTL 1.0 layout",
         "fields": radar_v1_fields},
        {"version": 2, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; no v1/v2 payload autodetection",
         "fields": radar_v2_fields},
        {"version": 3, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; v1/v2 byte-identical; no payload autodetection",
         "fields": radar_v3_fields},
        {"version": 4, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; v4 appends the authoritative radar visual group",
         "fields": radar_v4_fields},
    ]
    radar_contacts["phase3_live_record_version"] = 4

    target_presence = registries["TargetStatePresence"]
    target_presence["reserved"]["known_mask"] = 0x1FFFF
    target_presence["reserved"]["reserved_mask"] = 0xFFFFFFFFFFFE0000
    target_presence["values"].append({
        "bit": 14,
        "name": "EXACT_HUD_SPEED",
        "value": 0x4000,
        "source": {"document": p3_doc04_path, "section": "5.2"},
    })
    target_presence["values"].append({
        "bit": 16,
        "name": "HUD_TARGET_COLOR",
        "value": 0x10000,
        "source": {"document": p3_doc04_path, "section": "5.2"},
    })
    target_presence["values"].append({
        "bit": 15,
        "name": "HUD_TYPE_LABEL",
        "value": 0x8000,
        "source": {"document": p3_doc04_path, "section": "5.2"},
    })
    target_presence["values"].sort(key=lambda item: int(item["value"]))

    target_state = next(record for record in records if int(record["id"]) == 16)
    target_v1_fields = json.loads(json.dumps(target_state["fields"]))
    target_v2_fields = json.loads(json.dumps(target_v1_fields))
    target_v2_fields.append({
        "constraint": "[0;1,0e12] HUD speed units",
        "name": "exact_hud_speed",
        "nature": "A",
        "position": "25",
        "presence_condition": {"bits": [14], "selector": "presence"},
        "semantics": "bit 14; exact target-box display speed after HUD multiplier",
        "wire": "float32",
    })
    target_v3_fields = json.loads(json.dumps(target_v2_fields))
    target_v3_fields.append({
        "constraint": "UTF-8 1..255 octets",
        "name": "hud_type_label",
        "nature": "A",
        "position": "26",
        "presence_condition": {"bits": [15], "selector": "presence"},
        "semantics": "bit 15; exact second line rendered by the FSO Target Box",
        "wire": "utf8-string",
    })
    target_v4_fields = json.loads(json.dumps(target_v3_fields))
    target_v4_fields.append({
        "constraint": "RGBA bright final rÃ©solu par FSO",
        "name": "hud_target_color",
        "nature": "A",
        "position": str(len(target_v4_fields) + 1),
        "presence_condition": {"bits": [16], "selector": "presence"},
        "semantics": "bit 16; couleur HUD brillante autoritaire de la cible",
        "wire": "rgba8",
    })
    for index, field in enumerate(target_v2_fields, start=1):
        field["position"] = str(index)
    target_state["versions"] = [
        {"version": 1, "compatibility": "legacy FSTL 1.1 capture layout",
         "fields": target_v1_fields},
        {"version": 2, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; no v1/v2 payload autodetection",
         "fields": target_v2_fields},
        {"version": 3, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; v3 adds the conditional HUD target type label",
         "fields": target_v3_fields},
        {"version": 4, "minimum_minor": 1,
         "required_profile": "CockpitSensors",
         "compatibility": "explicit; v4 appends the authoritative HUD target color",
         "fields": target_v4_fields},
    ]
    target_state["phase3_live_record_version"] = 4

    records.append({
        "id": 29,
        "name": "HUD_ALERT_STATE",
        "version": 1,
        "scope": "player",
        "delta_atom": "entity_id",
        "create_delete": "no",
        "containers": ["FULL_SNAPSHOT", "DELTA"],
        "minimum_minor": 1,
        "required_profile": "CockpitSensors",
        "source": {"document": p3_doc04_path, "section": "10"},
        "fields": [
            {"position": "1", "name": "entity_id", "wire": "u64", "constraint": "nonzero", "nature": "A", "semantics": "observed player"},
            {"position": "2", "name": "presence", "wire": "u64", "constraint": "HudAlertStatePresence", "nature": "A", "semantics": "conditional active warning group"},
            {"position": "3", "name": "producer_sample_time_us", "wire": "u64", "constraint": "monotonic sample time", "nature": "A", "semantics": "flightHz HUD sample"},
            {"position": "4", "name": "primary_fire_threat_active", "wire": "bool8", "constraint": "0 or 1", "nature": "A", "semantics": "independent primary-fire threat lamp"},
            {"position": "5", "name": "missile_lock_state", "wire": "HudAlertMissileLockState", "constraint": "closed enum", "nature": "A", "semantics": "independent missile lock lamp state"},
            {"position": "6", "name": "warning_kind", "wire": "HudAlertWarningKind", "constraint": "closed enum", "nature": "A", "presence_condition": {"bits": [0], "selector": "presence"}, "semantics": "accepted active HUD warning"},
            {"position": "7", "name": "warning_instance_id", "wire": "u64", "constraint": "nonzero", "nature": "A", "presence_condition": {"bits": [0], "selector": "presence"}, "semantics": "changes only on accepted warning"},
            {"position": "8", "name": "warning_remaining_us", "wire": "u64", "constraint": "positive duration", "nature": "A", "presence_condition": {"bits": [0], "selector": "presence"}, "semantics": "remaining game time at sample"},
            {"position": "9", "name": "warning_text", "wire": "str<511>", "constraint": "UTF-8 1..511 bytes", "nature": "A", "presence_condition": {"bits": [0], "selector": "presence"}, "semantics": "exact localized accepted HUD text"},
        ],
    })
    records.sort(key=lambda record: int(record["id"]))

    correspondence = schema.get("cpp_correspondence")
    if not isinstance(correspondence, dict):
        raise SchemaError("base schema C++ correspondence is missing")
    correspondence["verified_constant_count"] = 182
    correspondence["verified_enum_count"] = 146
    correspondence["document_registry_count"] = 138
    correspondence["bound_registry_count"] = 138

    validate_schema_shape(schema)
    return schema


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--write", action="store_true", help="regenerate only the additive FSTL 1.1 schema")
    action.add_argument("--check", action="store_true", help="verify checked-in schemas (default)")
    parser.add_argument("--self-test", action="store_true", help="run negative drift/collision checks after verification")
    parser.add_argument("--wire-version", choices=("all", "1.0", "1.1"), default="all")
    parser.add_argument("--schema", type=Path, help="override one selected schema path")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.schema is not None and args.wire_version == "all":
            raise SchemaError("--schema requires --wire-version 1.0 or 1.1")
        if args.write and args.wire_version != "1.1":
            raise SchemaError(
                "ordinary write mode is forbidden for frozen FSTL 1.0; "
                "select --wire-version 1.1"
            )

        frozen_schema = load_frozen_v1_0_schema()
        schema = build_fstl_v1_1_schema()
        rendered_v1_1 = render_schema(schema)
        v1_0_path = (args.schema.resolve() if args.schema is not None and args.wire_version == "1.0"
                     else SCHEMA_V1_0_PATH)
        v1_1_path = (args.schema.resolve() if args.schema is not None and args.wire_version == "1.1"
                     else SCHEMA_V1_1_PATH)

        if args.wire_version in ("all", "1.0"):
            data = v1_0_path.read_bytes()
            if (len(data) != FROZEN_SCHEMA_V1_0_BYTES or
                    hashlib.sha256(data).hexdigest() != FROZEN_SCHEMA_V1_0_SHA256):
                raise SchemaError(f"frozen FSTL 1.0 schema drift: {v1_0_path}")
        if args.wire_version in ("all", "1.1"):
            if args.write:
                write_schema(rendered_v1_1, v1_1_path)
            check_schema(rendered_v1_1, v1_1_path)

        negative_tests: tuple[str, ...] = ()
        vector_self_tests: list[str] = []
        if args.self_test:
            negative_tests = run_negative_self_tests(schema)
            if args.wire_version in ("all", "1.0"):
                vector_self_tests.append(run_schema_vector_self_test(v1_0_path))
            if args.wire_version in ("all", "1.1"):
                vector_self_tests.append(run_schema_vector_self_test(v1_1_path))
    except (OSError, SchemaError, json.JSONDecodeError) as exc:
        print(f"FSTL schema verification failed: {exc}", file=sys.stderr)
        return 1

    print(
        "FSTL schemas verified: frozen 1.0 plus additive 1.1; "
        f"{len(schema['message_types'])} messages, "
        f"{len(schema['record_types'])} records, "
        f"{len(schema['capabilities'])} capabilities, "
        f"{len(schema['validation_errors'])} validation errors, "
        f"{len(schema['numeric_registries'])} numeric registries "
        f"({schema['cpp_correspondence']['bound_registry_count']} C++-bound)."
    )
    if negative_tests:
        print(f"FSTL negative self-tests passed: {', '.join(negative_tests)}.")
    for vector_self_test in vector_self_tests:
        print(vector_self_test)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

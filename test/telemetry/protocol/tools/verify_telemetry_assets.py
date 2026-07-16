#!/usr/bin/env python3
"""Verify Phase 0 vector coverage and archived deterministic transport seeds."""

from __future__ import annotations

import argparse
import json
import re
import sys
import hashlib
from pathlib import Path
from typing import Any, Iterable


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root is not an object: {path}")
    return value


def collect_numeric_values(value: Any, names: set[str]) -> set[int]:
    found: set[int] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = re.sub(r"[^a-z]", "", key.lower())
            if normalized in names:
                candidates: Iterable[Any] = child if isinstance(child, list) else (child,)
                for candidate in candidates:
                    if isinstance(candidate, int) and not isinstance(candidate, bool):
                        found.add(candidate)
            found.update(collect_numeric_values(child, names))
    elif isinstance(value, list):
        for child in value:
            found.update(collect_numeric_values(child, names))
    return found


def verify_fixture(
    metadata_path: Path,
    metadata: dict[str, Any],
    expected_root: Path,
    validation_names: dict[int, str],
) -> list[str]:
    errors: list[str] = []
    input_files = metadata.get("inputFiles")
    if not isinstance(input_files, list) or not input_files:
        return errors
    for input_name in input_files:
        if not isinstance(input_name, str) or not (metadata_path.parent / input_name).is_file():
            errors.append(f"missing vector input {input_name!r} referenced by {metadata_path}")

    valid = metadata.get("valid")
    validation_error = metadata.get("expectedValidationError")
    validation_name = metadata.get("expectedValidationErrorName")
    if validation_error not in validation_names or validation_name != validation_names.get(validation_error):
        errors.append(f"ValidationError code/name drift in {metadata_path}")
    if valid is True:
        if validation_error != 0:
            errors.append(f"valid fixture has a non-zero validation error: {metadata_path}")
        canonical = metadata.get("expectedCanonicalJson")
        if not isinstance(canonical, str):
            errors.append(f"valid fixture has no expectedCanonicalJson: {metadata_path}")
        elif not (expected_root / canonical).is_file() and not (metadata_path.parent / canonical).is_file():
            errors.append(f"missing canonical JSON {canonical!r} referenced by {metadata_path}")
    elif valid is False:
        if not isinstance(validation_error, int) or validation_error == 0:
            errors.append(f"invalid fixture has no non-zero validation error: {metadata_path}")
        if "expectedCanonicalJson" in metadata:
            errors.append(f"invalid fixture has expectedCanonicalJson: {metadata_path}")
        if not isinstance(metadata.get("invalidCategory"), str):
            errors.append(f"invalid fixture has no invalidCategory: {metadata_path}")
        if metadata.get("kind") in ("message-payload", "record"):
            replay = metadata.get("cxxReplay")
            if not isinstance(replay, dict) or replay.get("status") not in ("exact", "divergence"):
                errors.append(f"invalid protocol fixture has no explicit C++ replay disposition: {metadata_path}")
    else:
        errors.append(f"fixture has no strict valid boolean: {metadata_path}")
    return errors


def normalize_hex(value: str) -> str:
    return re.sub(r"[^0-9a-f]", "", value.lower().removeprefix("0x"))


def verify_transport_seeds(repo: Path, manifest_path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    manifest = load_object(manifest_path)
    seeds = manifest.get("seeds")
    if not isinstance(seeds, list) or not seeds:
        return [], [f"seed manifest has no seeds: {manifest_path}"]
    errors: list[str] = []
    verified: list[dict[str, Any]] = []
    for seed in seeds:
        if not isinstance(seed, dict):
            errors.append(f"invalid seed entry in {manifest_path}")
            continue
        source_name = seed.get("source")
        literal = seed.get("literal")
        if not isinstance(source_name, str) or not isinstance(literal, str):
            errors.append(f"seed entry lacks source/literal: {seed!r}")
            continue
        source = (repo / source_name).resolve()
        if not source.is_file():
            errors.append(f"seed source is missing: {source_name}")
            continue
        normalized_source = normalize_hex(source.read_text(encoding="utf-8"))
        normalized_literal = normalize_hex(literal)
        if not normalized_literal or normalized_literal not in normalized_source:
            errors.append(f"seed literal {literal} is not present in {source_name}")
            continue
        verified.append(seed)
    return verified, errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--schema", type=Path)
    parser.add_argument("--vectors", type=Path)
    parser.add_argument("--expected", type=Path)
    parser.add_argument("--seeds", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = args.repo.resolve()
    protocol_root = repo / "test" / "telemetry" / "protocol"
    schema_path = (args.schema or protocol_root / "schema" / "fstl-v1.yaml").resolve()
    vectors = (args.vectors or protocol_root / "vectors").resolve()
    expected = (args.expected or protocol_root / "expected").resolve()
    seeds_path = (args.seeds or protocol_root / "transport-harness-seeds.json").resolve()

    try:
        schema = load_object(schema_path)
        message_ids = {int(entry["id"]) for entry in schema["message_types"]}
        record_ids = {int(entry["id"]) for entry in schema["record_types"]}
        validation_names = {int(entry["id"]): str(entry["name"]) for entry in schema["validation_errors"]}
        covered_messages: set[int] = set()
        covered_records: set[int] = set()
        fixture_errors: list[str] = []
        fixture_count = 0
        valid_count = 0
        invalid_count = 0
        invalid_categories: set[str] = set()
        for metadata_path in sorted(vectors.rglob("*.json")):
            metadata = load_object(metadata_path)
            if not isinstance(metadata.get("inputFiles"), list):
                continue
            fixture_count += 1
            valid_count += metadata.get("valid") is True
            invalid_count += metadata.get("valid") is False
            fixture_errors.extend(verify_fixture(metadata_path, metadata, expected, validation_names))
            if metadata.get("valid") is False and isinstance(metadata.get("invalidCategory"), str):
                invalid_categories.add(metadata["invalidCategory"])
            covered_messages.update(
                collect_numeric_values(metadata, {"messagetype", "messagetypes"})
            )
            covered_records.update(collect_numeric_values(metadata, {"recordtype", "recordtypes"}))
            canonical = metadata.get("expectedCanonicalJson")
            if isinstance(canonical, str):
                canonical_path = expected / canonical
                if not canonical_path.is_file():
                    canonical_path = metadata_path.parent / canonical
                if canonical_path.is_file():
                    canonical_json = load_object(canonical_path)
                    covered_messages.update(
                        collect_numeric_values(canonical_json, {"messagetype", "messagetypes"})
                    )
                    covered_records.update(
                        collect_numeric_values(canonical_json, {"recordtype", "recordtypes"})
                    )

        verified_seeds, seed_errors = verify_transport_seeds(repo, seeds_path)
        catalogue_coverage = load_object(protocol_root / "protocol-coverage.json")
        amendment_manifest = load_object(protocol_root / "fstl-1.1-vectors.manifest.json")
        amendment_errors: list[str] = []
        for entry in amendment_manifest.get("files", []):
            candidate = protocol_root / entry["path"]
            if not candidate.is_file() or hashlib.sha256(candidate.read_bytes()).hexdigest() != entry["sha256"]:
                amendment_errors.append(f"FSTL 1.1 vector drift: {entry['path']}")
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"Telemetry asset verification failed: {error}", file=sys.stderr)
        return 1

    missing_messages = sorted(message_ids - covered_messages)
    missing_records = sorted(record_ids - covered_records)
    report = {
        "schema": "FSTL-1.0-coverage",
        "fixtures": {"total": fixture_count, "valid": valid_count, "invalid": invalid_count},
        "invalidCategories": {"covered": len(invalid_categories), "names": sorted(invalid_categories)},
        "messages": {
            "expected": len(message_ids),
            "covered": len(message_ids & covered_messages),
            "missingIds": missing_messages,
        },
        "records": {
            "expected": len(record_ids),
            "covered": len(record_ids & covered_records),
            "missingIds": missing_records,
        },
        "transportHarnessSeeds": {
            "verified": len(verified_seeds),
            "names": [str(seed.get("name", "")) for seed in verified_seeds],
        },
    }
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    errors = fixture_errors + seed_errors + amendment_errors
    if args.require_complete and missing_messages:
        errors.append(f"missing message vector coverage for IDs: {missing_messages}")
    if args.require_complete and missing_records:
        errors.append(f"missing record vector coverage for IDs: {missing_records}")
    if args.require_complete and len(catalogue_coverage.get("validCatalogue", [])) != 14:
        errors.append("§10.4 machine-readable catalogue coverage is not 14/14")
    if args.require_complete and len(catalogue_coverage.get("invalidCatalogue", [])) != 20:
        errors.append("§10.5 machine-readable catalogue coverage is not 20/20")

    print(
        "FSTL asset coverage: "
        f"fixtures={fixture_count} (valid={valid_count}, invalid={invalid_count}), "
        f"messages={len(message_ids & covered_messages)}/{len(message_ids)}, "
        f"records={len(record_ids & covered_records)}/{len(record_ids)}, "
        f"invalid-categories={len(invalid_categories)}, "
        f"transport-seeds={len(verified_seeds)}."
    )
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

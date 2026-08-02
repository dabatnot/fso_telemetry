#!/usr/bin/env python3
"""Create and validate requirement-centric telemetry implementation trackers."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "fs2open.telemetry.product-implementation.v1"
TRACKER_DIRECTORY = Path("documentation/analysis/implementation-status")
PHASE_DIRECTORY = re.compile(r"^(?P<number>[0-9]+)-.+$")
REQUIREMENT_ID = re.compile(r"^P(?P<phase>[0-9]+)-(?:REQ|F|NF)-[0-9]{3}$")
OBSERVATION_ID = re.compile(r"^P(?P<phase>[0-9]+)-OBS-[0-9]{2}$")
EMPTY_CELL = {"", "-", "—", "–", "none", "aucune"}
IMPLEMENTATION_STATES = {"pending", "in_progress", "blocked", "implemented"}
EVIDENCE_STATES = {"pending", "passed", "failed", "blocked"}
BLOCKER_CATEGORIES = {"product", "tooling", "human"}
FORBIDDEN_INPUT_PARTS = {".git", "archive", "implementation-status"}

TOP_LEVEL_KEYS = {
    "schema",
    "phase",
    "phaseDirectory",
    "contractFingerprint",
    "updatedAtUtc",
    "requirements",
    "observations",
}
REQUIREMENT_KEYS = {
    "id",
    "observable",
    "expectedAutomatedProof",
    "implementationStatus",
    "implementationSummary",
    "implementationPaths",
    "proofs",
    "requiredObservations",
    "blocker",
}
PROOF_KEYS = {
    "id",
    "kind",
    "command",
    "status",
    "inputs",
    "inputFingerprint",
    "observedAtUtc",
    "result",
    "resultRef",
}
OBSERVATION_KEYS = {
    "id",
    "purpose",
    "expected",
    "status",
    "inputs",
    "inputFingerprint",
    "observedAtUtc",
    "reviewedBy",
    "observed",
    "deviation",
    "impact",
}
BLOCKER_KEYS = {"category", "summary", "nextAction"}


class TrackerError(RuntimeError):
    pass


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Manage the active telemetry phase implementation tracker."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def common_phase(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--repo-root", type=Path, default=Path.cwd())
        subparser.add_argument("--phase-number", type=int, required=True)
        subparser.add_argument("--phase-directory", type=Path)

    initialize = subparsers.add_parser(
        "init", help="Create a pending tracker from the canonical document 07."
    )
    common_phase(initialize)
    initialize.add_argument("--tracker", type=Path)

    validate = subparsers.add_parser(
        "validate", help="Validate a tracker and derive implementation completeness."
    )
    common_phase(validate)
    validate.add_argument("--tracker", type=Path)
    validate.add_argument("--require-complete", action="store_true")
    validate.add_argument("--json", action="store_true", dest="as_json")

    fingerprint = subparsers.add_parser(
        "fingerprint", help="Fingerprint explicit repository-relative evidence inputs."
    )
    fingerprint.add_argument("--repo-root", type=Path, default=Path.cwd())
    fingerprint.add_argument("--path", action="append", required=True, dest="paths")
    fingerprint.add_argument("--json", action="store_true", dest="as_json")

    contract = subparsers.add_parser(
        "contract-fingerprint", help="Print the active contract fingerprint."
    )
    common_phase(contract)
    contract.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def normalize_cell(cell: str) -> str:
    return cell.strip()


def cell_is_empty(cell: str) -> bool:
    normalized = re.sub(r"[`*_]", "", cell).strip().lower()
    return normalized in EMPTY_CELL


def split_markdown_row(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def ensure_within(path: Path, parent: Path, label: str) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(parent.resolve())
    except ValueError as error:
        raise TrackerError(f"{label} must stay under {parent}") from error
    return resolved


def discover_phase_directory(
    root: Path, phase_number: int, supplied: Path | None
) -> Path:
    specs_root = (root / "documentation" / "analysis" / "specs").resolve()
    if supplied is not None:
        candidate = supplied if supplied.is_absolute() else root / supplied
        candidate = ensure_within(candidate, specs_root, "phase directory")
        if not candidate.is_dir():
            raise TrackerError(f"missing phase directory: {candidate}")
        if not candidate.name.startswith(f"{phase_number}-"):
            raise TrackerError(
                f"phase directory {candidate.name} does not match phase {phase_number}"
            )
        return candidate

    matches = sorted(
        path
        for path in specs_root.iterdir()
        if path.is_dir() and path.name.startswith(f"{phase_number}-")
    )
    if len(matches) != 1:
        raise TrackerError(
            f"expected one active directory for phase {phase_number}, found {len(matches)}"
        )
    return matches[0].resolve()


def phase_delivery_file(phase_directory: Path) -> Path:
    matches = sorted(phase_directory.glob("07-*.md"))
    if len(matches) != 1:
        raise TrackerError(
            f"expected one 07 delivery document in {phase_directory}, found {len(matches)}"
        )
    return matches[0]


def parse_contract(
    phase_directory: Path, phase_number: int
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    delivery = phase_delivery_file(phase_directory)
    try:
        lines = delivery.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise TrackerError(f"{delivery} is not valid UTF-8") from error

    requirements: list[dict[str, Any]] = []
    observations: list[dict[str, str]] = []
    for line_number, line in enumerate(lines, start=1):
        cells = split_markdown_row(line) if line.lstrip().startswith("|") else []
        if not cells:
            continue
        requirement_match = re.fullmatch(r"`([^`]+)`", cells[0])
        if requirement_match and REQUIREMENT_ID.fullmatch(requirement_match.group(1)):
            requirement_id = requirement_match.group(1)
            if int(REQUIREMENT_ID.fullmatch(requirement_id).group("phase")) != phase_number:
                raise TrackerError(
                    f"{delivery.name}:{line_number}: foreign requirement {requirement_id}"
                )
            if len(cells) != 4:
                raise TrackerError(
                    f"{delivery.name}:{line_number}: requirement row must have 4 cells"
                )
            observable = normalize_cell(cells[1])
            automated = "" if cell_is_empty(cells[2]) else normalize_cell(cells[2])
            manual = "" if cell_is_empty(cells[3]) else normalize_cell(cells[3])
            required_observations = re.findall(
                rf"P{phase_number}-OBS-[0-9]{{2}}", manual
            )
            if manual and not required_observations:
                raise TrackerError(
                    f"{delivery.name}:{line_number}: manual evidence must reference "
                    "a canonical observation ID"
                )
            requirements.append(
                {
                    "id": requirement_id,
                    "observable": observable,
                    "automated": automated,
                    "requiredObservations": required_observations,
                }
            )
            continue

        observation_match = re.fullmatch(r"`([^`]+)`", cells[0])
        if observation_match and OBSERVATION_ID.fullmatch(observation_match.group(1)):
            observation_id = observation_match.group(1)
            if int(OBSERVATION_ID.fullmatch(observation_id).group("phase")) != phase_number:
                raise TrackerError(
                    f"{delivery.name}:{line_number}: foreign observation {observation_id}"
                )
            if len(cells) != 3:
                raise TrackerError(
                    f"{delivery.name}:{line_number}: observation row must have 3 cells"
                )
            observations.append(
                {
                    "id": observation_id,
                    "purpose": normalize_cell(cells[1]),
                    "expected": normalize_cell(cells[2]),
                }
            )

    requirement_ids = [item["id"] for item in requirements]
    observation_ids = [item["id"] for item in observations]
    duplicates = [
        item
        for item, count in Counter(requirement_ids + observation_ids).items()
        if count > 1
    ]
    if duplicates:
        raise TrackerError("duplicate canonical IDs: " + ", ".join(sorted(duplicates)))
    if not requirements:
        raise TrackerError(f"{delivery.name}: no canonical requirement")

    defined_observations = set(observation_ids)
    referenced_observations = {
        observation
        for requirement in requirements
        for observation in requirement["requiredObservations"]
    }
    missing = sorted(referenced_observations - defined_observations)
    unused = sorted(defined_observations - referenced_observations)
    if missing:
        raise TrackerError("undefined observations: " + ", ".join(missing))
    if unused:
        raise TrackerError("unreferenced observations: " + ", ".join(unused))
    return requirements, observations


def relative_to_root(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def contract_files(root: Path, phase_number: int) -> list[Path]:
    files: set[Path] = set()
    analysis = root / "documentation" / "analysis"
    for path in analysis.glob("*.md"):
        if path.name != "04-implementation-roadmap.md":
            files.add(path.resolve())

    specs = analysis / "specs"
    for candidate in specs.iterdir():
        if not candidate.is_dir():
            continue
        match = PHASE_DIRECTORY.fullmatch(candidate.name)
        if match and int(match.group("number")) <= phase_number:
            files.update(path.resolve() for path in candidate.rglob("*") if path.is_file())

    for relative in (
        Path("test/telemetry/protocol/schema"),
        Path("test/telemetry/protocol/vectors"),
    ):
        base = root / relative
        if base.is_dir():
            files.update(path.resolve() for path in base.rglob("*") if path.is_file())

    return sorted(files, key=lambda item: relative_to_root(root, item))


def hash_files(root: Path, files: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda item: relative_to_root(root, item)):
        relative = relative_to_root(root, path)
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return "sha256:" + digest.hexdigest()


def current_contract_fingerprint(root: Path, phase_number: int) -> str:
    files = contract_files(root, phase_number)
    if not files:
        raise TrackerError("active contract contains no files")
    return hash_files(root, files)


def path_is_forbidden(relative: Path) -> bool:
    lowered = [part.lower() for part in relative.parts]
    if any(part in FORBIDDEN_INPUT_PARTS for part in lowered):
        return True
    return bool(lowered and lowered[0].startswith("build"))


def expand_input_paths(root: Path, raw_paths: Any) -> tuple[list[str], list[Path]]:
    if not isinstance(raw_paths, list) or not raw_paths:
        raise TrackerError("evidence inputs must be a non-empty list")

    normalized: list[str] = []
    files: set[Path] = set()
    for raw in raw_paths:
        if not isinstance(raw, str) or not raw.strip():
            raise TrackerError("evidence input paths must be non-empty strings")
        source = Path(raw)
        if source.is_absolute():
            raise TrackerError(f"evidence input must be repository-relative: {raw}")
        resolved = (root / source).resolve()
        try:
            relative = resolved.relative_to(root.resolve())
        except ValueError as error:
            raise TrackerError(f"evidence input escapes repository: {raw}") from error
        if path_is_forbidden(relative):
            raise TrackerError(f"evidence input is not an active source path: {raw}")
        if not resolved.exists():
            raise TrackerError(f"evidence input does not exist: {raw}")
        normalized.append(relative.as_posix())
        if resolved.is_file():
            files.add(resolved)
        elif resolved.is_dir():
            for candidate in resolved.rglob("*"):
                if not candidate.is_file():
                    continue
                candidate_relative = candidate.resolve().relative_to(root.resolve())
                if not path_is_forbidden(candidate_relative):
                    files.add(candidate.resolve())
        else:
            raise TrackerError(f"unsupported evidence input: {raw}")
    if not files:
        raise TrackerError("evidence inputs resolve to no files")
    return normalized, sorted(files, key=lambda item: relative_to_root(root, item))


def fingerprint_inputs(root: Path, raw_paths: Any) -> tuple[str, set[str]]:
    _, files = expand_input_paths(root, raw_paths)
    relative_files = {relative_to_root(root, path) for path in files}
    return hash_files(root, files), relative_files


def canonical_tracker_path(root: Path, phase_number: int) -> Path:
    return (root / TRACKER_DIRECTORY / f"phase-{phase_number}.json").resolve()


def resolve_tracker_path(
    root: Path, phase_number: int, supplied: Path | None
) -> Path:
    if supplied is None:
        return canonical_tracker_path(root, phase_number)
    candidate = supplied if supplied.is_absolute() else root / supplied
    return ensure_within(candidate, root, "tracker")


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def valid_timestamp(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return value.endswith("Z")


def initial_tracker(
    root: Path,
    phase_number: int,
    phase_directory: Path,
    requirements: list[dict[str, Any]],
    observations: list[dict[str, str]],
) -> dict[str, Any]:
    requirement_entries = []
    for requirement in requirements:
        proofs = []
        if requirement["automated"]:
            proofs.append(
                {
                    "id": f"{requirement['id']}-AUTO-01",
                    "kind": "automated",
                    "command": None,
                    "status": "pending",
                    "inputs": [],
                    "inputFingerprint": None,
                    "observedAtUtc": None,
                    "result": None,
                    "resultRef": None,
                }
            )
        requirement_entries.append(
            {
                "id": requirement["id"],
                "observable": requirement["observable"],
                "expectedAutomatedProof": requirement["automated"],
                "implementationStatus": "pending",
                "implementationSummary": None,
                "implementationPaths": [],
                "proofs": proofs,
                "requiredObservations": requirement["requiredObservations"],
                "blocker": None,
            }
        )

    observation_entries = [
        {
            "id": observation["id"],
            "purpose": observation["purpose"],
            "expected": observation["expected"],
            "status": "pending",
            "inputs": [],
            "inputFingerprint": None,
            "observedAtUtc": None,
            "reviewedBy": None,
            "observed": None,
            "deviation": None,
            "impact": None,
        }
        for observation in observations
    ]
    return {
        "schema": SCHEMA,
        "phase": phase_number,
        "phaseDirectory": relative_to_root(root, phase_directory),
        "contractFingerprint": current_contract_fingerprint(root, phase_number),
        "updatedAtUtc": utc_now(),
        "requirements": requirement_entries,
        "observations": observation_entries,
    }


def require_exact_keys(
    value: Any, expected: set[str], label: str, errors: list[str]
) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return False
    actual = set(value)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing:
        errors.append(f"{label} missing fields: {', '.join(missing)}")
    if extra:
        errors.append(f"{label} unknown fields: {', '.join(extra)}")
    return not missing and not extra


def non_empty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def validate_blocker(
    blocker: Any, label: str, errors: list[str]
) -> bool:
    if blocker is None:
        return False
    if not require_exact_keys(blocker, BLOCKER_KEYS, label, errors):
        return True
    if blocker.get("category") not in BLOCKER_CATEGORIES:
        errors.append(f"{label}.category is invalid")
    for field in ("summary", "nextAction"):
        if not non_empty_string(blocker.get(field)):
            errors.append(f"{label}.{field} must be non-empty")
    return True


def validate_tracker(
    root: Path,
    tracker_path: Path,
    phase_number: int,
    phase_directory: Path,
    requirements: list[dict[str, Any]],
    observations: list[dict[str, str]],
) -> dict[str, Any]:
    errors: list[str] = []
    gaps: list[str] = []
    try:
        tracker = json.loads(tracker_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {
            "phase": phase_number,
            "derivedStatus": "invalid",
            "errors": [f"missing tracker: {relative_to_root(root, tracker_path)}"],
            "gaps": [],
        }
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return {
            "phase": phase_number,
            "derivedStatus": "invalid",
            "errors": [f"unreadable tracker: {error}"],
            "gaps": [],
        }

    if not require_exact_keys(tracker, TOP_LEVEL_KEYS, "tracker", errors):
        return {"derivedStatus": "invalid", "errors": errors, "gaps": gaps}
    if tracker.get("schema") != SCHEMA:
        errors.append(f"tracker.schema must be {SCHEMA}")
    if tracker.get("phase") != phase_number:
        errors.append(f"tracker.phase must be {phase_number}")
    expected_directory = relative_to_root(root, phase_directory)
    if tracker.get("phaseDirectory") != expected_directory:
        errors.append(f"tracker.phaseDirectory must be {expected_directory}")
    if not valid_timestamp(tracker.get("updatedAtUtc")):
        errors.append("tracker.updatedAtUtc must be an ISO-8601 UTC timestamp")

    expected_contract_fingerprint = current_contract_fingerprint(root, phase_number)
    if tracker.get("contractFingerprint") != expected_contract_fingerprint:
        gaps.append("contract fingerprint is stale")

    tracker_requirements = tracker.get("requirements")
    tracker_observations = tracker.get("observations")
    if not isinstance(tracker_requirements, list):
        errors.append("tracker.requirements must be a list")
        tracker_requirements = []
    if not isinstance(tracker_observations, list):
        errors.append("tracker.observations must be a list")
        tracker_observations = []

    expected_requirement_ids = [item["id"] for item in requirements]
    actual_requirement_ids = [
        item.get("id") if isinstance(item, dict) else None
        for item in tracker_requirements
    ]
    if actual_requirement_ids != expected_requirement_ids:
        errors.append("tracker requirements must exactly match canonical order and IDs")

    expected_observation_ids = [item["id"] for item in observations]
    actual_observation_ids = [
        item.get("id") if isinstance(item, dict) else None
        for item in tracker_observations
    ]
    if actual_observation_ids != expected_observation_ids:
        errors.append("tracker observations must exactly match canonical order and IDs")

    proof_ids: set[str] = set()
    requirement_by_id = {item["id"]: item for item in requirements}
    for index, entry in enumerate(tracker_requirements):
        label = f"requirements[{index}]"
        if not require_exact_keys(entry, REQUIREMENT_KEYS, label, errors):
            continue
        requirement_id = entry["id"]
        expected = requirement_by_id.get(requirement_id)
        if expected is None:
            continue
        if entry["observable"] != expected["observable"]:
            errors.append(f"{requirement_id}: observable differs from document 07")
        if entry["expectedAutomatedProof"] != expected["automated"]:
            errors.append(
                f"{requirement_id}: expected automated proof differs from document 07"
            )
        if entry["requiredObservations"] != expected["requiredObservations"]:
            errors.append(
                f"{requirement_id}: required observations differ from document 07"
            )

        state = entry["implementationStatus"]
        if state not in IMPLEMENTATION_STATES:
            errors.append(f"{requirement_id}: invalid implementationStatus")
            state = "pending"
        has_blocker = validate_blocker(
            entry["blocker"], f"{requirement_id}.blocker", errors
        )
        if state == "blocked" and not has_blocker:
            errors.append(f"{requirement_id}: blocked state requires a blocker")
        if state != "blocked" and has_blocker:
            gaps.append(f"{requirement_id}: blocker remains open")

        implementation_files: set[str] = set()
        implementation_paths = entry["implementationPaths"]
        if not isinstance(implementation_paths, list):
            errors.append(f"{requirement_id}: implementationPaths must be a list")
            implementation_paths = []
        elif implementation_paths:
            try:
                _, files = expand_input_paths(root, implementation_paths)
                implementation_files = {
                    relative_to_root(root, path) for path in files
                }
            except TrackerError as error:
                gaps.append(f"{requirement_id}: {error}")

        proofs = entry["proofs"]
        if not isinstance(proofs, list):
            errors.append(f"{requirement_id}: proofs must be a list")
            proofs = []
        passed_proofs = 0
        covered_files: set[str] = set()
        for proof_index, proof in enumerate(proofs):
            proof_label = f"{requirement_id}.proofs[{proof_index}]"
            if not require_exact_keys(proof, PROOF_KEYS, proof_label, errors):
                continue
            proof_id = proof["id"]
            if not non_empty_string(proof_id) or not proof_id.startswith(
                f"{requirement_id}-"
            ):
                errors.append(f"{proof_label}.id must be scoped to {requirement_id}")
            elif proof_id in proof_ids:
                errors.append(f"duplicate proof ID: {proof_id}")
            else:
                proof_ids.add(proof_id)
            if proof["kind"] not in {"automated", "static"}:
                errors.append(f"{proof_label}.kind is invalid")
            proof_state = proof["status"]
            if proof_state not in EVIDENCE_STATES:
                errors.append(f"{proof_label}.status is invalid")
                continue
            if proof_state == "pending":
                continue
            if not non_empty_string(proof["command"]):
                errors.append(f"{proof_label}.command must be non-empty")
            if not valid_timestamp(proof["observedAtUtc"]):
                errors.append(f"{proof_label}.observedAtUtc must be UTC")
            if not non_empty_string(proof["result"]):
                errors.append(f"{proof_label}.result must be non-empty")
            try:
                actual_fingerprint, input_files = fingerprint_inputs(
                    root, proof["inputs"]
                )
            except TrackerError as error:
                gaps.append(f"{proof_label}: {error}")
                continue
            if proof["inputFingerprint"] != actual_fingerprint:
                gaps.append(f"{proof_label}: input fingerprint is stale")
                continue
            if proof_state == "passed":
                passed_proofs += 1
                covered_files.update(input_files)
            else:
                gaps.append(f"{proof_label}: status is {proof_state}")

        if state != "implemented":
            gaps.append(f"{requirement_id}: implementationStatus is {state}")
            continue
        if not non_empty_string(entry["implementationSummary"]):
            errors.append(f"{requirement_id}: implemented state requires a summary")
        if not implementation_paths:
            errors.append(
                f"{requirement_id}: implemented state requires implementationPaths"
            )
        if has_blocker:
            gaps.append(f"{requirement_id}: implemented state has an open blocker")
        if expected["automated"] and passed_proofs == 0:
            gaps.append(f"{requirement_id}: no current passed automated proof")
        missing_coverage = sorted(implementation_files - covered_files)
        if expected["automated"] and missing_coverage:
            gaps.append(
                f"{requirement_id}: passed proofs do not cover implementation inputs: "
                + ", ".join(missing_coverage)
            )

    observation_by_id = {item["id"]: item for item in observations}
    for index, entry in enumerate(tracker_observations):
        label = f"observations[{index}]"
        if not require_exact_keys(entry, OBSERVATION_KEYS, label, errors):
            continue
        observation_id = entry["id"]
        expected = observation_by_id.get(observation_id)
        if expected is None:
            continue
        if entry["purpose"] != expected["purpose"]:
            errors.append(f"{observation_id}: purpose differs from document 07")
        if entry["expected"] != expected["expected"]:
            errors.append(f"{observation_id}: expected result differs from document 07")
        state = entry["status"]
        if state not in EVIDENCE_STATES:
            errors.append(f"{observation_id}: invalid status")
            continue
        if state != "passed":
            gaps.append(f"{observation_id}: observation status is {state}")
            continue
        for field in ("reviewedBy", "observed", "deviation", "impact"):
            if not non_empty_string(entry[field]):
                errors.append(f"{observation_id}: {field} must be non-empty")
        if not valid_timestamp(entry["observedAtUtc"]):
            errors.append(f"{observation_id}: observedAtUtc must be UTC")
        try:
            actual_fingerprint, _ = fingerprint_inputs(root, entry["inputs"])
        except TrackerError as error:
            gaps.append(f"{observation_id}: {error}")
            continue
        if entry["inputFingerprint"] != actual_fingerprint:
            gaps.append(f"{observation_id}: input fingerprint is stale")

    derived = "invalid" if errors else ("incomplete" if gaps else "complete")
    return {
        "schema": SCHEMA,
        "phase": phase_number,
        "tracker": relative_to_root(root, tracker_path),
        "derivedStatus": derived,
        "requirementCount": len(requirements),
        "observationCount": len(observations),
        "errors": errors,
        "gaps": gaps,
    }


def print_validation(result: dict[str, Any], as_json: bool) -> None:
    if as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    print(
        "telemetry implementation tracker: "
        f"phase={result.get('phase', '?')}, "
        f"status={result['derivedStatus']}, "
        f"requirements={result.get('requirementCount', '?')}, "
        f"observations={result.get('observationCount', '?')}"
    )
    for error in result["errors"]:
        print(f"error: {error}", file=sys.stderr)
    for gap in result["gaps"]:
        print(f"incomplete: {gap}")


def main() -> int:
    args = parse_arguments()
    root = args.repo_root.resolve()
    if not root.is_dir():
        print(f"missing repository root: {root}", file=sys.stderr)
        return 2

    try:
        if args.command == "fingerprint":
            fingerprint, files = fingerprint_inputs(root, args.paths)
            if args.as_json:
                print(
                    json.dumps(
                        {"fingerprint": fingerprint, "files": sorted(files)},
                        ensure_ascii=False,
                        indent=2,
                    )
                )
            else:
                print(fingerprint)
            return 0

        phase_directory = discover_phase_directory(
            root, args.phase_number, args.phase_directory
        )
        requirements, observations = parse_contract(
            phase_directory, args.phase_number
        )
        if args.command == "contract-fingerprint":
            fingerprint = current_contract_fingerprint(root, args.phase_number)
            if args.as_json:
                print(
                    json.dumps(
                        {
                            "phase": args.phase_number,
                            "fingerprint": fingerprint,
                            "fileCount": len(contract_files(root, args.phase_number)),
                        },
                        indent=2,
                    )
                )
            else:
                print(fingerprint)
            return 0

        tracker_path = resolve_tracker_path(
            root, args.phase_number, getattr(args, "tracker", None)
        )
        if args.command == "init":
            if tracker_path.exists():
                raise TrackerError(
                    f"tracker already exists and will not be overwritten: {tracker_path}"
                )
            tracker = initial_tracker(
                root,
                args.phase_number,
                phase_directory,
                requirements,
                observations,
            )
            tracker_path.parent.mkdir(parents=True, exist_ok=True)
            tracker_path.write_text(
                json.dumps(tracker, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            print(
                "initialized telemetry implementation tracker: "
                f"{relative_to_root(root, tracker_path)} "
                f"({len(requirements)} requirements, {len(observations)} observations)"
            )
            return 0

        result = validate_tracker(
            root,
            tracker_path,
            args.phase_number,
            phase_directory,
            requirements,
            observations,
        )
        print_validation(result, args.as_json)
        if result["errors"]:
            return 2
        if args.require_complete and result["gaps"]:
            return 1
        return 0
    except (OSError, TrackerError) as error:
        print(f"phase tracker error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Inventory active telemetry phases and locate their implementation tracker."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path


PHASE_DIRECTORY = re.compile(r"^(?P<number>[0-9]+)-.+$")
REQUIREMENT_ROW = re.compile(
    r"^\|\s*`(?P<id>P[0-9]+-(?:REQ|F|NF)-[0-9]{3})`\s*\|"
)
OBSERVATION_ROW = re.compile(
    r"^\|\s*`(?P<id>P[0-9]+-OBS-[0-9]{2})`\s*\|"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inventory the last active telemetry phase and its successor."
    )
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def numbered_documents(phase_directory: Path) -> tuple[dict[str, Path], list[str]]:
    errors: list[str] = []
    markdown = sorted(phase_directory.glob("*.md"))
    documents: dict[str, Path] = {}
    for path in markdown:
        key = "README" if path.name == "README.md" else path.name[:2]
        if key in documents:
            errors.append(f"duplicate document slot {key}: {path.name}")
        documents[key] = path
    expected = {"README", "01", "02", "03", "04", "05", "06", "07"}
    missing = sorted(expected - documents.keys())
    unexpected = sorted(documents.keys() - expected)
    if missing:
        errors.append(f"missing document slots: {', '.join(missing)}")
    if unexpected:
        errors.append(f"unexpected document slots: {', '.join(unexpected)}")
    if len(markdown) != 8:
        errors.append(f"expected 8 Markdown documents, found {len(markdown)}")
    return documents, errors


def main() -> int:
    args = parse_arguments()
    root = args.repo_root.resolve()
    specs = root / "documentation" / "analysis" / "specs"
    errors: list[str] = []
    if not specs.is_dir():
        print(f"missing active specs directory: {specs}", file=sys.stderr)
        return 2

    phases: dict[int, Path] = {}
    for candidate in sorted(specs.iterdir()):
        if not candidate.is_dir():
            continue
        match = PHASE_DIRECTORY.fullmatch(candidate.name)
        if not match:
            continue
        number = int(match.group("number"))
        if number in phases:
            errors.append(
                f"duplicate phase {number}: {phases[number].name}, {candidate.name}"
            )
        phases[number] = candidate

    if not phases:
        errors.append("no numbered active phase found")
        last_number = -1
        last_directory = None
        requirements: list[str] = []
        observations: list[str] = []
    else:
        numbers = sorted(phases)
        expected_numbers = list(range(numbers[0], numbers[-1] + 1))
        if numbers[0] != 0 or numbers != expected_numbers:
            errors.append(f"phase sequence is not contiguous from 0: {numbers}")
        last_number = numbers[-1]
        last_directory = phases[last_number]
        documents, document_errors = numbered_documents(last_directory)
        errors.extend(document_errors)
        requirements = []
        observations = []
        if "07" in documents:
            try:
                delivery = documents["07"].read_text(encoding="utf-8")
            except UnicodeDecodeError:
                errors.append(f"{documents['07']}: not valid UTF-8")
                delivery = ""
            requirements = [
                match.group("id")
                for line in delivery.splitlines()
                if (match := REQUIREMENT_ROW.match(line))
            ]
            observations = [
                match.group("id")
                for line in delivery.splitlines()
                if (match := OBSERVATION_ROW.match(line))
            ]
            duplicate_requirements = sorted(
                item for item, count in Counter(requirements).items() if count > 1
            )
            duplicate_observations = sorted(
                item for item, count in Counter(observations).items() if count > 1
            )
            if not requirements:
                errors.append("document 07 contains no canonical requirement row")
            if duplicate_requirements:
                errors.append(
                    "duplicate canonical requirements: "
                    + ", ".join(duplicate_requirements)
                )
            if duplicate_observations:
                errors.append(
                    "duplicate observation definitions: "
                    + ", ".join(duplicate_observations)
                )
            wrong_requirements = [
                item for item in requirements if not item.startswith(f"P{last_number}-")
            ]
            wrong_observations = [
                item for item in observations if not item.startswith(f"P{last_number}-")
            ]
            if wrong_requirements:
                errors.append(
                    "foreign requirement rows in last phase: "
                    + ", ".join(wrong_requirements)
                )
            if wrong_observations:
                errors.append(
                    "foreign observation rows in last phase: "
                    + ", ".join(wrong_observations)
                )

    tracker_relative = (
        Path("documentation")
        / "analysis"
        / "implementation-status"
        / f"phase-{last_number}.json"
        if last_number >= 0
        else None
    )
    result = {
        "schema": "telemetry-next-phase-inventory-v2",
        "repositoryRoot": str(root),
        "lastSpecifiedPhase": last_number if last_number >= 0 else None,
        "lastPhaseDirectory": (
            str(last_directory.relative_to(root)) if last_directory else None
        ),
        "nextPhase": last_number + 1,
        "requirementCount": len(set(requirements)),
        "requirementIds": sorted(set(requirements)),
        "observationCount": len(set(observations)),
        "observationIds": sorted(set(observations)),
        "implementationTracker": (
            tracker_relative.as_posix() if tracker_relative else None
        ),
        "implementationTrackerExists": (
            (root / tracker_relative).is_file() if tracker_relative else False
        ),
        "readiness": "tracker-validation-required",
        "errors": errors,
    }

    if args.as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(f"last specified phase: {result['lastSpecifiedPhase']}")
        print(f"last phase directory: {result['lastPhaseDirectory']}")
        print(f"canonical requirements: {result['requirementCount']}")
        print(f"defined observations: {result['observationCount']}")
        print(f"implementation tracker: {result['implementationTracker']}")
        print(
            "implementation tracker exists: "
            f"{str(result['implementationTrackerExists']).lower()}"
        )
        print(f"next phase if product-complete: {result['nextPhase']}")
        print("readiness: tracker validation required; inventory never grants readiness")
        for error in errors:
            print(f"error: {error}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())

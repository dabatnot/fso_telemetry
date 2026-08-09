#!/usr/bin/env python3
"""Mechanically validate one product-oriented telemetry phase specification."""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from collections import Counter
from pathlib import Path
from urllib.parse import unquote


REQUIREMENT_ROW = re.compile(
    r"^\|\s*`(?P<id>P[0-9]+-REQ-[0-9]{3})`\s*\|"
)
OBSERVATION_ROW = re.compile(
    r"^\|\s*`(?P<id>P[0-9]+-OBS-[0-9]{2})`\s*\|"
)
LINK = re.compile(r"!?\[[^\]]*\]\((?P<target>[^)]+)\)")
PLACEHOLDER = re.compile(r"\b(?:TODO|TBD)\b|\[PLACEHOLDER\]|<slug>", re.IGNORECASE)
RETIRED_STRUCTURE = (
    re.compile(r"P[0-9]+-W" + r"P-[0-9]+", re.IGNORECASE),
    re.compile(r"\bG[0-9]+-[A-Z]\b"),
    re.compile("certification-budget-" + "minutes", re.IGNORECASE),
    re.compile("gate-" + "certification", re.IGNORECASE),
    re.compile("certification" + "Eligible", re.IGNORECASE),
)
EMPTY_PROOF = {"", "-", "—", "–", "none", "aucune"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate one active telemetry phase specification."
    )
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--phase-number", type=int, required=True)
    parser.add_argument("--phase-directory", type=Path, required=True)
    return parser.parse_args()


def github_slug(heading: str) -> str:
    text = re.sub(r"[`*_~]", "", heading.strip().lower())
    text = "".join(
        character
        for character in text
        if character in {" ", "-", "_"} or character.isalnum()
    )
    return re.sub(r"\s", "-", text)


def heading_anchors(text: str) -> set[str]:
    counts: Counter[str] = Counter()
    anchors: set[str] = set()
    for line in text.splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        base = github_slug(match.group(1))
        suffix = counts[base]
        counts[base] += 1
        anchors.add(base if suffix == 0 else f"{base}-{suffix}")
    return anchors


def split_markdown_row(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def proof_is_empty(cell: str) -> bool:
    normalized = re.sub(r"[`*_]", "", cell).strip().lower()
    return normalized in EMPTY_PROOF


def main() -> int:
    args = parse_arguments()
    root = args.repo_root.resolve()
    phase_directory = (
        args.phase_directory
        if args.phase_directory.is_absolute()
        else root / args.phase_directory
    ).resolve()
    errors: list[str] = []

    specs_root = (root / "documentation" / "analysis" / "specs").resolve()
    if not phase_directory.is_dir():
        print(f"missing phase directory: {phase_directory}", file=sys.stderr)
        return 2
    try:
        phase_directory.relative_to(specs_root)
    except ValueError:
        print("phase directory must stay under active specs", file=sys.stderr)
        return 2
    if not re.match(rf"^{args.phase_number}-", phase_directory.name):
        errors.append(
            f"directory {phase_directory.name} does not match phase {args.phase_number}"
        )

    markdown = sorted(phase_directory.glob("*.md"))
    slots: dict[str, Path] = {}
    for path in markdown:
        slot = "README" if path.name == "README.md" else path.name[:2]
        if slot in slots:
            errors.append(f"duplicate document slot {slot}: {path.name}")
        slots[slot] = path
    expected_slots = {"README", "01", "02", "03", "04", "05", "06", "07"}
    missing_slots = sorted(expected_slots - slots.keys())
    unexpected_slots = sorted(slots.keys() - expected_slots)
    if len(markdown) != 8:
        errors.append(f"expected 8 Markdown documents, found {len(markdown)}")
    if missing_slots:
        errors.append(f"missing document slots: {', '.join(missing_slots)}")
    if unexpected_slots:
        errors.append(f"unexpected document slots: {', '.join(unexpected_slots)}")

    texts: dict[Path, str] = {}
    for path in markdown:
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            errors.append(f"{path.name}: not valid UTF-8")
            continue
        texts[path] = text
        if PLACEHOLDER.search(text):
            errors.append(f"{path.name}: unresolved placeholder")
        for line_number, line in enumerate(text.splitlines(), start=1):
            if any(pattern.search(line) for pattern in RETIRED_STRUCTURE):
                errors.append(
                    f"{path.name}:{line_number}: retired process structure"
                )

    if "README" in slots and slots["README"] in texts:
        readme = unicodedata.normalize("NFKD", texts[slots["README"]])
        readme_ascii = readme.encode("ascii", "ignore").decode("ascii").lower()
        for heading in ("resultat produit attendu", "frontiere de phase"):
            if heading not in readme_ascii:
                errors.append(f"README.md: missing product heading '{heading}'")

    requirements: list[str] = []
    observations: list[str] = []
    referenced_observations: set[str] = set()
    if "07" in slots and slots["07"] in texts:
        delivery = texts[slots["07"]]
        normalized_delivery = unicodedata.normalize("NFKD", delivery)
        delivery_ascii = (
            normalized_delivery.encode("ascii", "ignore").decode("ascii").lower()
        )
        for heading in (
            "table canonique des exigences",
            "observations neutres",
            "decision de livraison",
        ):
            if heading not in delivery_ascii:
                errors.append(f"{slots['07'].name}: missing section '{heading}'")

        for line_number, line in enumerate(delivery.splitlines(), start=1):
            requirement_match = REQUIREMENT_ROW.match(line)
            if requirement_match:
                requirement_id = requirement_match.group("id")
                requirements.append(requirement_id)
                cells = split_markdown_row(line)
                if len(cells) < 4:
                    errors.append(
                        f"{slots['07'].name}:{line_number}: incomplete traceability row"
                    )
                    continue
                observable, automated, manual = cells[1], cells[2], cells[3]
                if not observable or proof_is_empty(observable):
                    errors.append(
                        f"{slots['07'].name}:{line_number}: missing product observable"
                    )
                if proof_is_empty(automated) and proof_is_empty(manual):
                    errors.append(
                        f"{slots['07'].name}:{line_number}: no proof or observation"
                    )
                row_observations = set(
                    re.findall(rf"P{args.phase_number}-OBS-[0-9]{{2}}", manual)
                )
                if not proof_is_empty(manual) and not row_observations:
                    errors.append(
                        f"{slots['07'].name}:{line_number}: manual evidence must "
                        "reference a canonical observation ID"
                    )
                referenced_observations.update(row_observations)
            observation_match = OBSERVATION_ROW.match(line)
            if observation_match:
                observations.append(observation_match.group("id"))

    duplicate_requirements = sorted(
        item for item, count in Counter(requirements).items() if count > 1
    )
    duplicate_observations = sorted(
        item for item, count in Counter(observations).items() if count > 1
    )
    if not requirements:
        errors.append("document 07 contains no canonical requirement")
    if duplicate_requirements:
        errors.append(
            "duplicate canonical requirements: " + ", ".join(duplicate_requirements)
        )
    if duplicate_observations:
        errors.append(
            "duplicate observation definitions: " + ", ".join(duplicate_observations)
        )

    expected_prefix = f"P{args.phase_number}-REQ-"
    foreign_requirements = [
        item for item in requirements if not item.startswith(expected_prefix)
    ]
    if foreign_requirements:
        errors.append(
            "foreign requirement rows: " + ", ".join(foreign_requirements)
        )
    if requirements and not foreign_requirements:
        ordinals = sorted(int(item.rsplit("-", 1)[1]) for item in requirements)
        expected_ordinals = list(range(1, len(ordinals) + 1))
        if ordinals != expected_ordinals:
            errors.append(
                f"requirement sequence must be contiguous from 001: {ordinals}"
            )

    expected_observation_prefix = f"P{args.phase_number}-OBS-"
    foreign_observations = [
        item for item in observations if not item.startswith(expected_observation_prefix)
    ]
    if foreign_observations:
        errors.append(
            "foreign observation definitions: " + ", ".join(foreign_observations)
        )
    missing_observations = sorted(referenced_observations - set(observations))
    if missing_observations:
        errors.append(
            "referenced observations are not defined: "
            + ", ".join(missing_observations)
        )
    unreferenced_observations = sorted(
        set(observations) - referenced_observations
    )
    if unreferenced_observations:
        errors.append(
            "defined observations are not linked from requirement rows: "
            + ", ".join(unreferenced_observations)
        )

    anchors = {path: heading_anchors(text) for path, text in texts.items()}
    for source, text in texts.items():
        for match in LINK.finditer(text):
            raw_target = match.group("target").strip()
            if raw_target.startswith("<") and raw_target.endswith(">"):
                raw_target = raw_target[1:-1]
            if re.match(r"^[a-z][a-z0-9+.-]*:", raw_target, re.IGNORECASE):
                continue
            if raw_target.startswith("#"):
                target_path = source
                anchor = unquote(raw_target[1:])
            else:
                path_part, separator, anchor = raw_target.partition("#")
                path_part = unquote(path_part.split(" ", 1)[0])
                target_path = (source.parent / path_part).resolve()
                anchor = unquote(anchor) if separator else ""
                if not target_path.exists():
                    errors.append(
                        f"{source.name}: broken link target '{raw_target}'"
                    )
                    continue
            if anchor and target_path.suffix.lower() == ".md":
                if target_path not in anchors:
                    try:
                        target_text = target_path.read_text(encoding="utf-8")
                    except (OSError, UnicodeDecodeError):
                        errors.append(
                            f"{source.name}: unreadable link target '{raw_target}'"
                        )
                        continue
                    anchors[target_path] = heading_anchors(target_text)
                if anchor not in anchors[target_path]:
                    errors.append(
                        f"{source.name}: broken anchor '#{anchor}' in {target_path.name}"
                    )

    if errors:
        print("telemetry product phase specification validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print(
        "telemetry product phase specification validation passed: "
        f"phase={args.phase_number}, documents={len(markdown)}, "
        f"requirements={len(requirements)}, observations={len(observations)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

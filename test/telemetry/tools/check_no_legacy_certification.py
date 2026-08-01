"""Fail when active telemetry paths reference the retired certification system."""

from __future__ import annotations

import re
import sys
from pathlib import Path


TEXT_SUFFIXES = {
    ".cmake",
    ".cpp",
    ".h",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".txt",
    ".yaml",
    ".yml",
}

ACTIVE_ROOTS = (
    ".agents",
    ".github/workflows",
    "CMakeLists.txt",
    "ci",
    "code/source_groups.cmake",
    "code/telemetry",
    "documentation/analysis",
    "test/src",
    "test/telemetry",
)

EXCLUDED_PARTS = {
    ".git",
    "archive",
    "legacy-certification-2026-07-31",
    "reports",
}

# Keep the forbidden spellings out of this source so the guard checks itself.
FORBIDDEN = (
    re.compile("certification" + "Eligible", re.IGNORECASE),
    re.compile(r"P[0-9]+-W" + r"P-[0-9]+", re.IGNORECASE),
    re.compile("G2" + "-F", re.IGNORECASE),
    re.compile("wp11-" + "loss-gate", re.IGNORECASE),
    re.compile("telemetry_native_" + "performance_runner", re.IGNORECASE),
    re.compile("telemetry_phase1_" + "reliability_harness", re.IGNORECASE),
    re.compile("telemetry_phase2_" + "loss_harness", re.IGNORECASE),
    re.compile("telemetry_phase2_" + "performance_harness", re.IGNORECASE),
    re.compile("phase2_" + "oracle_harness", re.IGNORECASE),
    re.compile("aggregate_phase2_" + "g2f_evidence", re.IGNORECASE),
    re.compile("run_phase2_" + r"(oracle|reliability|performance)_evidence", re.IGNORECASE),
    re.compile("Phase2Seam" + "TestDouble"),
    re.compile("set_phase2_" + "seam_test_double"),
    re.compile("phase2_gameplay_ab_" + "test_seam", re.IGNORECASE),
)

ARCHIVE_DEPENDENCY = re.compile(
    r"(legacy-certification-2026-07-31|tooling-snapshots)",
    re.IGNORECASE,
)


def iter_active_files(root: Path):
    for relative_root in ACTIVE_ROOTS:
        base = root / relative_root
        if not base.exists():
            continue
        if base.is_file():
            yield base.relative_to(root), base
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            relative = path.relative_to(root)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            yield relative, path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_no_legacy_certification.py <repository-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    failures: list[str] = []
    for relative, path in iter_active_files(root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            failures.append(f"{relative}: not valid UTF-8")
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            for pattern in FORBIDDEN:
                if pattern.search(line):
                    failures.append(
                        f"{relative}:{line_number}: retired certification token"
                    )
                    break
            if relative.suffix.lower() in {".cmake", ".yaml", ".yml"} and ARCHIVE_DEPENDENCY.search(line):
                failures.append(
                    f"{relative}:{line_number}: build/CI dependency on historical archive"
                )
            if (relative.as_posix() == "code/source_groups.cmake" and
                    "_test_" + "seam" in line):
                failures.append(
                    f"{relative}:{line_number}: test-only header in production source group"
                )
    if failures:
        print("active telemetry paths still reference retired certification machinery:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("no retired certification references in active telemetry paths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

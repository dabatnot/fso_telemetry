#!/usr/bin/env python3
"""Verify the immutable machine-readable FSTL 1.0 contract.

The historical ledger remains byte-frozen, including its documentary inventory,
but mutable product documents are not reopened or used as protocol gates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Sequence


EXPECTED_SCHEMA = "FSTL-1.0-FROZEN-ARTIFACTS"
EXPECTED_WIRE_VERSION = "1.0"
EXPECTED_FILE_COUNT = 438
EXPECTED_TREE_SHA256 = "9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d"
EXPECTED_LEDGER_BYTES = 131_635
EXPECTED_LEDGER_SHA256 = "800bc258719b7992b1ae38a30d8dc1f8a7373a5dd0415a618a0d4e62a47dbc92"
EXPECTED_IMMUTABLE_FILE_COUNT = 431
EXPECTED_IMMUTABLE_TREE_SHA256 = "bdca3b1e3b317b44abb9e4869041767e6edf499acb728728b2eeaa8892300ea6"

LEDGER_RELATIVE_PATH = Path("test/telemetry/protocol/fstl-1.0-artifacts.manifest.json")
PROTOCOL_ROOT = Path("test/telemetry/protocol")
MACHINE_ROOTS = (PROTOCOL_ROOT / "vectors", PROTOCOL_ROOT / "expected")
MACHINE_FILES = (
    PROTOCOL_ROOT / "protocol-vectors.manifest.json",
    PROTOCOL_ROOT / "transport-vectors.manifest.json",
    PROTOCOL_ROOT / "protocol-coverage.json",
    PROTOCOL_ROOT / "transport-harness-seeds.json",
    PROTOCOL_ROOT / "fuzz/fstl.dict",
)
FROZEN_SCHEMA = PROTOCOL_ROOT / "schema/fstl-v1.yaml"


class FreezeError(RuntimeError):
    """Raised when the frozen inventory or one of its bytes drifts."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_tree_sha256(entries: Iterable[tuple[str, bytes]]) -> str:
    body = b"".join(
        path.encode("utf-8") + b"\0" + sha256(data).encode("ascii") + b"\n"
        for path, data in sorted(entries)
    )
    return sha256(body)


def canonical_relative_path(path: Path) -> str:
    return path.as_posix()


def frozen_paths(repo: Path) -> list[Path]:
    paths = [FROZEN_SCHEMA]
    for root in MACHINE_ROOTS:
        absolute_root = repo / root
        if absolute_root.is_dir():
            paths.extend(path.relative_to(repo) for path in absolute_root.rglob("*") if path.is_file())
    paths.extend(MACHINE_FILES)
    return sorted(set(paths), key=canonical_relative_path)


def read_frozen_entries(repo: Path) -> list[tuple[str, bytes]]:
    entries: list[tuple[str, bytes]] = []
    for relative_path in frozen_paths(repo):
        path = repo / relative_path
        if not path.is_file():
            raise FreezeError(f"missing frozen artifact: {canonical_relative_path(relative_path)}")
        entries.append((canonical_relative_path(relative_path), path.read_bytes()))
    return entries


def validate_ledger_path(value: Any) -> str:
    if not isinstance(value, str) or not value:
        raise FreezeError("invalid empty ledger path")
    if "\\" in value:
        raise FreezeError(f"non-canonical ledger path: {value}")
    candidate = PurePosixPath(value)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise FreezeError(f"traversal ledger path rejected: {value}")
    if "." in candidate.parts or candidate.as_posix() != value:
        raise FreezeError(f"non-canonical ledger path: {value}")
    return value


def load_ledger(repo: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    ledger_path = repo / LEDGER_RELATIVE_PATH
    try:
        manifest = json.loads(ledger_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FreezeError(f"cannot read frozen ledger {LEDGER_RELATIVE_PATH.as_posix()}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise FreezeError("frozen ledger root is not an object")
    raw_files = manifest.get("files")
    if not isinstance(raw_files, list):
        raise FreezeError("frozen ledger files is not an array")

    files: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, raw_entry in enumerate(raw_files):
        if not isinstance(raw_entry, dict):
            raise FreezeError(f"frozen ledger entry {index} is not an object")
        path = validate_ledger_path(raw_entry.get("path"))
        if path in seen:
            raise FreezeError(f"duplicate ledger path: {path}")
        seen.add(path)
        size = raw_entry.get("bytes")
        digest = raw_entry.get("sha256")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise FreezeError(f"invalid byte count in frozen ledger: {path}")
        if not isinstance(digest, str) or len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
            raise FreezeError(f"invalid SHA-256 in frozen ledger: {path}")
        files.append({"path": path, "bytes": size, "sha256": digest})

    paths = [entry["path"] for entry in files]
    if paths != sorted(paths):
        raise FreezeError("frozen ledger paths are not sorted")
    return manifest, files


def verify(repo: Path) -> tuple[int, str]:
    ledger_data = (repo / LEDGER_RELATIVE_PATH).read_bytes()
    if len(ledger_data) != EXPECTED_LEDGER_BYTES or sha256(ledger_data) != EXPECTED_LEDGER_SHA256:
        raise FreezeError(
            "frozen ledger identity drift: "
            f"expected {EXPECTED_LEDGER_BYTES} bytes and SHA-256 {EXPECTED_LEDGER_SHA256}"
        )
    manifest, ledger_files = load_ledger(repo)
    if manifest.get("schema") != EXPECTED_SCHEMA:
        raise FreezeError(f"frozen ledger schema drift: {manifest.get('schema')!r}")
    if manifest.get("wireVersion") != EXPECTED_WIRE_VERSION:
        raise FreezeError(f"frozen ledger wireVersion drift: {manifest.get('wireVersion')!r}")
    if manifest.get("fileCount") != EXPECTED_FILE_COUNT or len(ledger_files) != EXPECTED_FILE_COUNT:
        raise FreezeError(
            f"frozen ledger file count drift: manifest={manifest.get('fileCount')!r}, "
            f"entries={len(ledger_files)}, expected={EXPECTED_FILE_COUNT}"
        )
    if manifest.get("treeSha256") != EXPECTED_TREE_SHA256:
        raise FreezeError(
            f"frozen ledger tree SHA-256 drift: {manifest.get('treeSha256')!r}; "
            f"expected {EXPECTED_TREE_SHA256}"
        )

    actual_entries = read_frozen_entries(repo)
    actual = {path: data for path, data in actual_entries}
    declared = {entry["path"]: entry for entry in ledger_files}
    undeclared = sorted(set(actual) - set(declared))
    if undeclared:
        raise FreezeError("\n".join(f"unexpected frozen artifact: {path}" for path in undeclared))

    drift: list[str] = []
    for path in sorted(actual):
        data = actual[path]
        entry = declared[path]
        if entry["bytes"] != len(data):
            drift.append(f"byte-count drift: {path}: {len(data)} != {entry['bytes']}")
        digest = sha256(data)
        if entry["sha256"] != digest:
            drift.append(f"SHA-256 drift: {path}: {digest} != {entry['sha256']}")
    if drift:
        raise FreezeError("\n".join(drift))

    actual_tree = canonical_tree_sha256(actual_entries)
    if len(actual_entries) != EXPECTED_IMMUTABLE_FILE_COUNT or actual_tree != EXPECTED_IMMUTABLE_TREE_SHA256:
        raise FreezeError(
            "immutable FSTL 1.0 artifact drift: "
            f"{len(actual_entries)} files and tree SHA-256 {actual_tree}; "
            f"expected {EXPECTED_IMMUTABLE_FILE_COUNT} files and {EXPECTED_IMMUTABLE_TREE_SHA256}"
        )
    return len(actual_entries), actual_tree


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("--repo", type=Path, required=True, help="repository root to verify")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    repo = args.repo.resolve()
    try:
        count, tree = verify(repo)
    except (FreezeError, OSError, UnicodeError, ValueError, TypeError) as exc:
        print(f"FSTL 1.0 freeze verification failed: {exc}", file=sys.stderr)
        return 1
    print(f"FSTL 1.0 frozen artifacts verified: {count} files, tree SHA-256 {tree}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

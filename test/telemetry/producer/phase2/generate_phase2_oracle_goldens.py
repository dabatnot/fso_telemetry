#!/usr/bin/env python3
"""Generate the checked Phase 2 dashboard inventories from frozen wire vectors."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
TOOLS = ROOT / "test/telemetry/protocol/tools"
CONSOLE = TOOLS / "fstl_console_client.py"
GOLDENS = Path(__file__).resolve().parent / "goldens"
SESSION_ID = 0x1122334455667788

sys.path.insert(0, str(TOOLS))
import fstl_reference_decoder as reference


def packet(message_type: int, payload: bytes, sequence: int, flags: int = 0) -> bytes:
    message_crc = reference.crc32_iso_hdlc(payload)
    prefix = struct.pack(
        "<IBBBBHHQIIqQIHHIII",
        0x4C545346, 1, 1, message_type, flags, 68, len(payload), SESSION_ID,
        sequence, 0, 0, 1_000_000 + sequence - 1, sequence, 0, 1, len(payload), 0, message_crc,
    )
    return prefix + struct.pack(
        "<I", reference.crc32_iso_hdlc(prefix + bytes(4) + payload)
    ) + payload


def manifest_payload() -> bytes:
    records = b"".join(
        (
            ROOT / "test/telemetry/protocol/vectors/valid/records"
            / name / f"{name}.bin"
        ).read_bytes()
        for name in ("class_manifest", "weapon_manifest")
    )
    return struct.pack(
        "<IHHI32sQHH", 1, 0, 1, len(records), hashlib.sha256(records).digest(),
        1_000_000, 1, 2,
    ) + records


def inventory(profile: str) -> list[dict[str, object]]:
    vector = "phase2-promotion" if profile == "core-gate" else "phase2-complete-ship"
    welcome = (
        ROOT / "test/telemetry/protocol/vectors-v1.1/welcome-accepted-minor-one"
        / "welcome-accepted-minor-one.payload.bin"
    ).read_bytes()
    session_begin = (
        ROOT / "test/telemetry/protocol/vectors/valid/messages/session_begin/session_begin.bin"
    ).read_bytes()
    snapshot = (
        ROOT / "test/telemetry/protocol/vectors-v1.1" / vector / f"{vector}.bin"
    ).read_bytes()
    datagrams = (
        packet(3, welcome, 1, 2),
        packet(4, session_begin, 2, 2),
        packet(5, manifest_payload(), 3, 2),
        packet(6, snapshot, 4, 2 | 4),
    )
    with tempfile.TemporaryDirectory() as temporary:
        paths = []
        for index, datagram in enumerate(datagrams):
            path = Path(temporary) / f"{index}.bin"
            path.write_bytes(datagram)
            paths.append(path)
        result = subprocess.run(
            [sys.executable, "-B", str(CONSOLE), "--replay", *(str(path) for path in paths),
             "--stale-ms", "100000000"],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
    if result.returncode != 0:
        raise RuntimeError(result.stderr)
    live = [json.loads(line) for line in result.stdout.splitlines()
            if json.loads(line).get("status") == "Live"]
    if len(live) != 1:
        raise RuntimeError(f"{profile}: expected exactly one Live publication")
    return live[0]["dashboard"]["inventory"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="replace checked goldens")
    args = parser.parse_args()
    GOLDENS.mkdir(parents=True, exist_ok=True)
    mismatches = []
    for profile in ("core-gate", "complete-ship"):
        value = json.dumps(inventory(profile), indent=2, sort_keys=True) + "\n"
        target = GOLDENS / f"oracle-inventory-{profile}.json"
        if args.write:
            target.write_text(value, encoding="utf-8")
        elif not target.is_file() or target.read_text(encoding="utf-8") != value:
            mismatches.append(str(target))
    if mismatches:
        print("stale Phase 2 oracle goldens:\n" + "\n".join(mismatches), file=sys.stderr)
        return 1
    print("Phase 2 oracle inventories are canonical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

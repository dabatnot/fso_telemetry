#!/usr/bin/env python3
"""Generate byte-authoritative FSTL 1.0 Phase 0 transport vectors.

This is intentionally independent from the C++ codec. It uses explicit
little-endian packing and Python's standard-library CRC implementation.
"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict


MAGIC = 0x4C545346
VERSION_MAJOR = 1
VERSION_MINOR = 0
HEADER_SIZE = 68
MAX_FRAGMENT_PAYLOAD = 1132
SESSION_ID = 0x1122334455667788

MESSAGE_DELTA = 7
MESSAGE_HEARTBEAT = 9
FLAG_FRAGMENTED = 0x01

ERROR_NONE = 0
ERROR_DATAGRAM_TOO_SHORT = 2
ERROR_RESERVED_HEADER_FLAG = 8
ERROR_BAD_DATAGRAM_LENGTH = 9
ERROR_BAD_DATAGRAM_CRC = 10
ERROR_BAD_FRAGMENT_COUNT = 16
ERROR_BAD_FRAGMENT_INDEX = 17
ERROR_BAD_FRAGMENT_OFFSET = 18
ERROR_BAD_FRAGMENT_SLICE = 19
ERROR_INCONSISTENT_FRAGMENT = 21
ERROR_BAD_MESSAGE_CRC = 23

HEADER_FORMAT = struct.Struct("<IBBBBHHQIIqQIHHIIII")
assert HEADER_FORMAT.size == HEADER_SIZE


@dataclass(frozen=True)
class Header:
    message_type: int
    flags: int
    payload_size: int
    session_id: int
    packet_sequence: int
    frame_id: int
    mission_time_us: int
    sent_time_us: int
    message_id: int
    fragment_index: int
    fragment_count: int
    message_size: int
    fragment_offset: int
    message_crc32: int
    crc32: int = 0


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def pack_header(header: Header) -> bytes:
    return HEADER_FORMAT.pack(
        MAGIC,
        VERSION_MAJOR,
        VERSION_MINOR,
        header.message_type,
        header.flags,
        HEADER_SIZE,
        header.payload_size,
        header.session_id,
        header.packet_sequence,
        header.frame_id,
        header.mission_time_us,
        header.sent_time_us,
        header.message_id,
        header.fragment_index,
        header.fragment_count,
        header.message_size,
        header.fragment_offset,
        header.message_crc32,
        header.crc32,
    )


def datagram(header: Header, payload: bytes) -> bytes:
    if len(payload) != header.payload_size:
        raise ValueError("payload size does not match header")
    zeroed = pack_header(replace(header, crc32=0)) + payload
    return pack_header(replace(header, crc32=crc32(zeroed))) + payload


def payload_bytes(size: int) -> bytes:
    return bytes(((0x31 + index * 37) & 0xFF) for index in range(size))


def fragment_message(size: int, message_id: int) -> Dict[str, bytes]:
    logical = payload_bytes(size)
    count = 1 if size == 0 else (size + MAX_FRAGMENT_PAYLOAD - 1) // MAX_FRAGMENT_PAYLOAD
    result: Dict[str, bytes] = {}
    logical_crc = crc32(logical)
    for index in range(count):
        offset = index * MAX_FRAGMENT_PAYLOAD
        payload = logical[offset : offset + MAX_FRAGMENT_PAYLOAD]
        message_type = MESSAGE_HEARTBEAT if size == 0 else MESSAGE_DELTA
        flags = FLAG_FRAGMENTED if count > 1 else 0
        header = Header(
            message_type=message_type,
            flags=flags,
            payload_size=len(payload),
            session_id=SESSION_ID,
            packet_sequence=1000 + index,
            frame_id=0 if size == 0 else 1,
            mission_time_us=0 if size == 0 else 1_000_000,
            sent_time_us=2_000_000 + index,
            message_id=message_id,
            fragment_index=index,
            fragment_count=count,
            message_size=size,
            fragment_offset=offset,
            message_crc32=logical_crc,
        )
        result[f"{index:03d}.bin"] = datagram(header, payload)
    return result


def metadata(
    name: str,
    valid: bool,
    message_type: int,
    inputs: Dict[str, bytes],
    expected_error: int,
    notes: str,
) -> bytes:
    value = {
        "schema": "FSTL-1.0",
        "name": name,
        "kind": "datagram-sequence",
        "valid": valid,
        "messageType": message_type,
        "inputFiles": sorted(inputs),
        "expectedValidationError": expected_error,
        "notes": notes,
    }
    if valid:
        value["expectedCanonicalJson"] = f"transport/{name}.json"
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("ascii")


def canonical(name: str, size: int, count: int, message_crc32: int) -> bytes:
    value = {
        "fragmentCount": count,
        "messageCrc32": f"0x{message_crc32:08x}",
        "messageSize": size,
        "name": name,
        "schema": "FSTL-1.0",
    }
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("ascii")


def add_fixture(
    files: Dict[str, bytes],
    category: str,
    name: str,
    inputs: Dict[str, bytes],
    valid: bool,
    message_type: int,
    expected_error: int,
    notes: str,
) -> None:
    base = f"vectors/{category}/datagrams/transport/{name}"
    for filename, contents in inputs.items():
        files[f"{base}/{filename}"] = contents
    files[f"{base}/{name}.json"] = metadata(
        name, valid, message_type, inputs, expected_error, notes
    )
    if valid:
        logical_size = sum(len(value) - HEADER_SIZE for value in inputs.values())
        count = len(inputs)
        logical_crc = 0 if logical_size == 0 else crc32(payload_bytes(logical_size))
        files[f"expected/transport/{name}.json"] = canonical(
            name, logical_size, count, logical_crc
        )


def decode_header(encoded: bytes) -> Header:
    fields = HEADER_FORMAT.unpack(encoded[:HEADER_SIZE])
    return Header(
        message_type=fields[3],
        flags=fields[4],
        payload_size=fields[6],
        session_id=fields[7],
        packet_sequence=fields[8],
        frame_id=fields[9],
        mission_time_us=fields[10],
        sent_time_us=fields[11],
        message_id=fields[12],
        fragment_index=fields[13],
        fragment_count=fields[14],
        message_size=fields[15],
        fragment_offset=fields[16],
        message_crc32=fields[17],
        crc32=fields[18],
    )


def mutated_datagram(source: bytes, **changes: int) -> bytes:
    header = replace(decode_header(source), **changes)
    payload = source[HEADER_SIZE:]
    return datagram(header, payload)


def build_files() -> Dict[str, bytes]:
    if crc32(b"123456789") != 0xCBF43926:
        raise RuntimeError("host CRC-32 does not match CRC-32/ISO-HDLC")

    files: Dict[str, bytes] = {}
    valid_vectors: Dict[int, Dict[str, bytes]] = {}
    for ordinal, size in enumerate((0, 1, 1132, 1133, 2264, 2265), start=1):
        inputs = fragment_message(size, 100 + ordinal)
        valid_vectors[size] = inputs
        name = f"transport_{size}_bytes"
        add_fixture(
            files,
            "valid",
            name,
            inputs,
            True,
            MESSAGE_HEARTBEAT if size == 0 else MESSAGE_DELTA,
            ERROR_NONE,
            f"Canonical transport fragmentation boundary for {size} logical bytes.",
        )

    empty_datagram = valid_vectors[0]["000.bin"]
    for size in range(HEADER_SIZE):
        name = f"truncated_header_{size:02d}"
        add_fixture(
            files,
            "invalid",
            name,
            {"000.bin": empty_datagram[:size]},
            False,
            MESSAGE_HEARTBEAT,
            ERROR_DATAGRAM_TOO_SHORT,
            f"Datagram truncated to {size} bytes.",
        )

    one = valid_vectors[1]["000.bin"]
    bad_crc = bytearray(one)
    bad_crc[-1] ^= 0x80
    add_fixture(
        files, "invalid", "bad_datagram_crc", {"000.bin": bytes(bad_crc)}, False,
        MESSAGE_DELTA, ERROR_BAD_DATAGRAM_CRC, "Payload changed without resealing datagram CRC."
    )

    wrong_message_crc = mutated_datagram(one, message_crc32=decode_header(one).message_crc32 ^ 1)
    add_fixture(
        files, "invalid", "bad_message_crc", {"000.bin": wrong_message_crc}, False,
        MESSAGE_DELTA, ERROR_BAD_MESSAGE_CRC, "Datagram CRC is valid but logical message CRC is not."
    )

    cases = (
        ("reserved_header_flag", {"flags": 0x80}, ERROR_RESERVED_HEADER_FLAG),
        ("zero_fragment_count", {"fragment_count": 0}, ERROR_BAD_FRAGMENT_COUNT),
        ("fragment_index_out_of_range", {"fragment_index": 1}, ERROR_BAD_FRAGMENT_INDEX),
        ("fragment_offset_noncanonical", {"fragment_offset": 1}, ERROR_BAD_FRAGMENT_OFFSET),
        ("fragment_slice_noncanonical", {"message_size": 2}, ERROR_BAD_FRAGMENT_SLICE),
    )
    for name, changes, expected in cases:
        add_fixture(
            files, "invalid", name, {"000.bin": mutated_datagram(one, **changes)}, False,
            MESSAGE_DELTA, expected, f"Transport mutation: {name.replace('_', ' ')}."
        )

    trailing = one + b"\x00"
    add_fixture(
        files, "invalid", "trailing_datagram_byte", {"000.bin": trailing}, False,
        MESSAGE_DELTA, ERROR_BAD_DATAGRAM_LENGTH, "Actual datagram length exceeds the declared length."
    )

    first_two = dict(valid_vectors[1133])
    second = first_two["001.bin"]
    first_two["001.bin"] = mutated_datagram(second, frame_id=2)
    add_fixture(
        files, "invalid", "inconsistent_fragment_metadata", first_two, False,
        MESSAGE_DELTA, ERROR_INCONSISTENT_FRAGMENT, "frame_id changes between fragments."
    )

    duplicate_source = valid_vectors[1133]["000.bin"]
    contradictory_payload = bytearray(duplicate_source[HEADER_SIZE:])
    contradictory_payload[0] ^= 0x40
    header = decode_header(duplicate_source)
    contradictory = datagram(header, bytes(contradictory_payload))
    add_fixture(
        files,
        "invalid",
        "contradictory_duplicate_fragment",
        {"000.bin": duplicate_source, "001.bin": contradictory},
        False,
        MESSAGE_DELTA,
        ERROR_INCONSISTENT_FRAGMENT,
        "Same fragment identity and metadata with different bytes.",
    )

    manifest_path = "transport-vectors.manifest.json"
    manifest = {
        "schema": "FSTL-1.0",
        "generator": "generate_transport_vectors.py",
        "files": sorted((*files.keys(), manifest_path)),
    }
    files[manifest_path] = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode("ascii")
    return files


def materialize(root: Path, files: Dict[str, bytes]) -> None:
    for relative, contents in files.items():
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents)


def owned_directories(root: Path) -> tuple[Path, Path, Path]:
    return (
        root / "vectors/valid/datagrams/transport",
        root / "vectors/invalid/datagrams/transport",
        root / "expected/transport",
    )


def compare(root: Path, expected: Dict[str, bytes]) -> list[str]:
    differences: list[str] = []
    for relative, contents in expected.items():
        path = root / relative
        if not path.exists():
            differences.append(f"missing: {relative}")
        elif path.read_bytes() != contents:
            differences.append(f"different: {relative}")
    actual_paths = {
        path.relative_to(root).as_posix()
        for directory in owned_directories(root)
        if directory.exists()
        for path in directory.rglob("*")
        if path.is_file()
    }
    actual_paths.add("transport-vectors.manifest.json")
    for relative in sorted(actual_paths - set(expected)):
        differences.append(f"unexpected: {relative}")
    return differences


def remove_previous_generated_files(root: Path) -> None:
    manifest_path = root / "transport-vectors.manifest.json"
    if not manifest_path.exists():
        return
    value = json.loads(manifest_path.read_text(encoding="ascii"))
    if value.get("generator") != "generate_transport_vectors.py":
        raise RuntimeError("refusing to use an unrecognized vector manifest")
    for relative in value.get("files", []):
        if not isinstance(relative, str):
            raise RuntimeError("invalid path in transport vector manifest")
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise RuntimeError("vector manifest contains an unowned path")
        candidate = (root / relative_path).resolve()
        manifest_file = (root / "transport-vectors.manifest.json").resolve()
        owned_roots = tuple(directory.resolve() for directory in owned_directories(root))
        allowed = candidate == manifest_file
        for owned_root in owned_roots:
            try:
                candidate.relative_to(owned_root)
                allowed = True
            except ValueError:
                pass
        if not allowed:
            raise RuntimeError("vector manifest path escapes its owned directories")
        if candidate.is_file():
            candidate.unlink()


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    files = build_files()
    if args.write:
        remove_previous_generated_files(root)
        materialize(root, files)
        return 0

    differences = compare(root, files)
    if differences:
        print("\n".join(differences))
        return 1
    print(f"verified {len(files)} transport vector files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

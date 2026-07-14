#!/usr/bin/env python3
"""Build deterministic, target-specific fuzz corpora from FSTL vectors."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TARGETS = (
    "fuzz_packet_reader",
    "fuzz_datagram",
    "fuzz_reassembler",
    "fuzz_control_payloads",
    "fuzz_records",
    "fuzz_transactions",
    "fuzz_comm_views",
    "fuzz_video_payloads",
)

CONTROL_MESSAGES = {1, 2, 3, 4, 9, 10, 11, 12, 13, 20}
TRANSACTION_MESSAGES = {5, 6, 7, 8}
VIDEO_MESSAGES = {14, 15, 16, 17, 18, 19}
COMM_RECORDS = {25, 26, 27}


@dataclass(frozen=True)
class Datagram:
    message_type: int
    flags: int
    fragment_index: int
    fragment_count: int
    message_size: int
    fragment_offset: int
    payload: bytes


class Corpus:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.entries: dict[str, dict[str, dict[str, Any]]] = {target: {} for target in TARGETS}

    def add(self, target: str, data: bytes, source: str) -> None:
        digest = hashlib.sha256(data).hexdigest()
        entry = self.entries[target].setdefault(digest, {"size": len(data), "sources": []})
        if source not in entry["sources"]:
            entry["sources"].append(source)
        destination = self.output / target / digest
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            destination.write_bytes(data)

    def ensure_nonempty(self) -> None:
        for target in TARGETS:
            if not self.entries[target]:
                self.add(target, b"", "synthetic:empty-input")

    def manifest(self, vectors: Path, metadata_count: int, input_count: int) -> dict[str, Any]:
        return {
            "schema": "FSTL-1.0-fuzz-corpus",
            "sourceVectors": "test/telemetry/protocol/vectors",
            "metadataCount": metadata_count,
            "inputFileCount": input_count,
            "targets": {
                target: {
                    "seedCount": len(entries),
                    "seeds": {digest: entries[digest] for digest in sorted(entries)},
                }
                for target, entries in self.entries.items()
            },
        }


def parse_datagram(data: bytes) -> Datagram | None:
    if len(data) < 68:
        return None
    header_size, payload_size = struct.unpack_from("<HH", data, 8)
    if header_size != 68 or payload_size > 1132 or len(data) < header_size + payload_size:
        return None
    fragment_index, fragment_count = struct.unpack_from("<HH", data, 48)
    message_size, fragment_offset = struct.unpack_from("<II", data, 52)
    return Datagram(
        message_type=data[6],
        flags=data[7],
        fragment_index=fragment_index,
        fragment_count=fragment_count,
        message_size=message_size,
        fragment_offset=fragment_offset,
        payload=data[header_size : header_size + payload_size],
    )


def logical_payload(datagrams: Iterable[Datagram]) -> tuple[int, int, bytes] | None:
    parts = list(datagrams)
    if not parts:
        return None
    first = parts[0]
    if first.message_size > 2_097_152 or any(
        part.message_type != first.message_type
        or part.fragment_count != first.fragment_count
        or part.message_size != first.message_size
        for part in parts
    ):
        return None
    output = bytearray(first.message_size)
    present = bytearray(first.message_size)
    for part in parts:
        end = part.fragment_offset + len(part.payload)
        if end > first.message_size:
            return None
        output[part.fragment_offset:end] = part.payload
        present[part.fragment_offset:end] = b"\x01" * len(part.payload)
    if first.message_size != 0 and any(value == 0 for value in present):
        return None
    return first.message_type, first.flags, bytes(output)


def add_record_region(corpus: Corpus, message_type: int, flags: int, payload: bytes, source: str) -> None:
    layouts = {
        5: (56, 54, 1),
        6: (60, 58, 2),
        7: (20, 16, 3),
    }
    layout = layouts.get(message_type)
    if message_type == 8 and len(payload) >= 24:
        layout = (24, 22, 5 if payload[20] == 2 else 4)
    if layout is None:
        return
    prefix_size, count_offset, container = layout
    if len(payload) < prefix_size:
        return
    record_count = struct.unpack_from("<H", payload, count_offset)[0]
    records = payload[prefix_size:]
    corpus.add("fuzz_records", bytes((0, container)) + struct.pack("<H", record_count) + records, source)
    add_individual_records(corpus, records, record_count, container, source)


def add_individual_records(
    corpus: Corpus, records: bytes, record_count: int, container: int, source: str
) -> None:
    offset = 0
    for index in range(record_count):
        if len(records) - offset < 6:
            return
        record_type, version, record_flags, length = struct.unpack_from("<HBBH", records, offset)
        offset += 6
        if length > len(records) - offset:
            return
        payload = records[offset : offset + length]
        offset += length
        direct = bytes((1,)) + struct.pack("<HBB", record_type, version, record_flags) + bytes((container, 0)) + payload
        record_source = f"{source}#record-{index}-{record_type}"
        corpus.add("fuzz_records", direct, record_source)
        add_comm_record(corpus, record_type, payload, record_source)


def add_comm_record(corpus: Corpus, record_type: int, payload: bytes, source: str) -> None:
    if record_type in COMM_RECORDS:
        corpus.add("fuzz_comm_views", bytes((record_type,)) + payload, source)


def normalize_record_vector(data: bytes, metadata: dict[str, Any]) -> tuple[int, int, int, bytes]:
    record_type = int(metadata.get("recordType", 0))
    version = int(metadata.get("recordVersion", 1))
    flags = int(metadata.get("recordFlags", 0))
    if len(data) >= 6:
        encoded_type, encoded_version, encoded_flags, length = struct.unpack_from("<HBBH", data, 0)
        if encoded_type == record_type and length == len(data) - 6:
            return encoded_type, encoded_version, encoded_flags, data[6:]
    return record_type, version, flags, data


def record_container(record_type: int, flags: int) -> int:
    if record_type in {3, 4, 25}:
        return 1
    if record_type in {27, 28}:
        return 5
    if flags != 0 and record_type in {5, 11, 18}:
        return 3
    return 2


def add_message_payload(corpus: Corpus, message_type: int, payload: bytes, source: str, flags: int = 0) -> None:
    wrapped = bytes((message_type & 0xFF,)) + payload
    if message_type in CONTROL_MESSAGES:
        corpus.add("fuzz_control_payloads", wrapped, source)
    if message_type in TRANSACTION_MESSAGES:
        corpus.add("fuzz_transactions", wrapped, source)
        if len(wrapped) <= 0xFFFF:
            corpus.add("fuzz_transactions", struct.pack("<H", len(wrapped)) + wrapped, f"{source}#stateful")
        add_record_region(corpus, message_type, flags, payload, source)
    if message_type in VIDEO_MESSAGES:
        corpus.add("fuzz_video_payloads", wrapped, source)


def safe_input_path(metadata_path: Path, value: str) -> Path:
    candidate = (metadata_path.parent / value).resolve()
    parent = metadata_path.parent.resolve()
    if candidate != parent and parent not in candidate.parents:
        raise ValueError(f"inputFiles entry escapes its fixture directory: {metadata_path}: {value}")
    return candidate


def prepare(vectors: Path, output: Path) -> dict[str, Any]:
    vectors = vectors.resolve()
    output = output.resolve()
    if not vectors.is_dir():
        raise ValueError(f"vector directory does not exist: {vectors}")
    if output == vectors or output in vectors.parents or vectors in output.parents:
        raise ValueError("corpus output must not contain, or be contained by, the checked-in vector directory")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    corpus = Corpus(output)
    metadata_count = 0
    input_count = 0
    for metadata_path in sorted(vectors.rglob("*.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if not isinstance(metadata, dict) or not isinstance(metadata.get("inputFiles"), list):
            continue
        metadata_count += 1
        inputs: list[tuple[str, bytes]] = []
        for input_name in metadata["inputFiles"]:
            if not isinstance(input_name, str):
                raise ValueError(f"non-string input file in {metadata_path}")
            input_path = safe_input_path(metadata_path, input_name)
            data = input_path.read_bytes()
            source = input_path.relative_to(vectors).as_posix()
            inputs.append((source, data))
            input_count += 1
            corpus.add("fuzz_packet_reader", data, source)

        kind = str(metadata.get("kind", ""))
        message_type = int(metadata.get("messageType", 0) or 0)
        if kind == "record":
            for source, data in inputs:
                record_type, version, flags, payload = normalize_record_vector(data, metadata)
                container = record_container(record_type, flags)
                direct = (
                    bytes((1,))
                    + struct.pack("<HBB", record_type, version, flags)
                    + bytes((container, 0))
                    + payload
                )
                corpus.add("fuzz_records", direct, source)
                add_comm_record(corpus, record_type, payload, source)
            continue

        if kind == "message-payload":
            for source, payload in inputs:
                add_message_payload(corpus, message_type, payload, source)
            continue

        if kind == "datagram-sequence" or "datagrams" in metadata_path.parts:
            sequence = bytearray()
            parsed: list[Datagram] = []
            for source, data in inputs:
                corpus.add("fuzz_datagram", data, source)
                if len(data) <= 0xFFFF:
                    sequence += struct.pack("<H", len(data)) + data
                candidate = parse_datagram(data)
                if candidate is not None:
                    parsed.append(candidate)
                    add_message_payload(corpus, candidate.message_type, candidate.payload, source, candidate.flags)
            if sequence:
                corpus.add("fuzz_reassembler", bytes(sequence), metadata_path.relative_to(vectors).as_posix())
            complete = logical_payload(parsed)
            if complete is not None:
                parsed_type, parsed_flags, payload = complete
                add_message_payload(
                    corpus,
                    message_type or parsed_type,
                    payload,
                    f"{metadata_path.relative_to(vectors).as_posix()}#logical",
                    parsed_flags,
                )

    corpus.ensure_nonempty()
    manifest = corpus.manifest(vectors, metadata_count, input_count)
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectors", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = prepare(args.vectors, args.output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"FSTL fuzz corpus generation failed: {error}")
        return 1
    counts = ", ".join(
        f"{target}={manifest['targets'][target]['seedCount']}" for target in TARGETS
    )
    print(
        f"FSTL fuzz corpus generated from {manifest['inputFileCount']} vector inputs: {counts}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

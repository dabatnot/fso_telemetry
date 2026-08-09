"""Durable indexed capture storage for the Simpit Lab.

The container is deliberately outside the FSTL wire contract.  It retains the
validated producer datagrams byte-for-byte while adding a compact time index,
mutable named ranges and crash-safe metadata.
"""

from __future__ import annotations

import base64
import bisect
import errno
import hashlib
import json
import os
import shutil
import sqlite3
import struct
import threading
import time
import uuid
from array import array
from collections import deque
from contextlib import closing
from pathlib import Path
from typing import Any, Iterable

try:  # Python 3.14+
    from compression import zstd

    def _compress(data: bytes) -> bytes:
        return zstd.compress(data, level=6)

    def _decompress(data: bytes) -> bytes:
        return zstd.decompress(data)
except ImportError:  # pragma: no cover - supported by the locked fallback dependency
    import zstandard

    _compressor = zstandard.ZstdCompressor(level=6)
    _decompressor = zstandard.ZstdDecompressor()

    def _compress(data: bytes) -> bytes:
        return _compressor.compress(data)

    def _decompress(data: bytes) -> bytes:
        return _decompressor.decompress(data)


CAPTURE_SCHEMA = "FSTL-simpit-capture-v3"
LEGACY_CAPTURE_SCHEMAS = ("FSTL-dashboard-capture-v1", "FSTL-dashboard-capture-v2")
APPLICATION_ID = 0x4653544C  # FSTL
USER_VERSION = 3
CHUNK_MAX_SPAN_US = 1_000_000
CHUNK_MAX_BYTES = 4 * 1024 * 1024
CHECKPOINT_INTERVAL_US = 5_000_000
DEFAULT_WARN_BYTES = 5 * 1024**3
DEFAULT_STOP_BYTES = 10 * 1024**3
DEFAULT_FREE_RESERVE_BYTES = 2 * 1024**3
_PACKET_PREFIX = struct.Struct("<QQHI")  # relative, observed monotonic, UTC bytes, datagram bytes


def _utc_now() -> str:
    from datetime import datetime, timezone

    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def default_capture_directory() -> Path:
    return Path.home() / "Documents" / "FSO Simpit Lab" / "Captures"


def _database_size(path: Path) -> int:
    total = 0
    for suffix in ("", "-wal", "-shm"):
        candidate = Path(str(path) + suffix)
        try:
            total += candidate.stat().st_size
        except FileNotFoundError:
            pass
    return total


def _connect(path: Path, *, writable: bool) -> sqlite3.Connection:
    if writable:
        connection = sqlite3.connect(path, timeout=10.0, check_same_thread=False)
    else:
        connection = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=10.0)
    connection.row_factory = sqlite3.Row
    return connection


def _create_schema(connection: sqlite3.Connection) -> None:
    connection.executescript(
        f"""
        PRAGMA application_id={APPLICATION_ID};
        PRAGMA user_version={USER_VERSION};
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;
        CREATE TABLE IF NOT EXISTS metadata(
            key TEXT PRIMARY KEY,
            value_json TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS chunks(
            chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,
            first_timeline_us INTEGER NOT NULL,
            last_timeline_us INTEGER NOT NULL,
            packet_count INTEGER NOT NULL,
            uncompressed_bytes INTEGER NOT NULL,
            compressed_bytes INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            codec TEXT NOT NULL CHECK(codec='zstd'),
            payload BLOB NOT NULL
        );
        CREATE INDEX IF NOT EXISTS chunks_time ON chunks(first_timeline_us,last_timeline_us);
        CREATE TABLE IF NOT EXISTS sessions(
            session_index INTEGER PRIMARY KEY,
            session_id TEXT NOT NULL,
            start_us INTEGER NOT NULL,
            end_us INTEGER,
            reason TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS checkpoints(
            checkpoint_id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_index INTEGER NOT NULL,
            timeline_us INTEGER NOT NULL,
            state_blob BLOB NOT NULL,
            sha256 TEXT NOT NULL,
            UNIQUE(session_index,timeline_us)
        );
        CREATE INDEX IF NOT EXISTS checkpoints_time ON checkpoints(session_index,timeline_us);
        CREATE TABLE IF NOT EXISTS ranges(
            range_id TEXT PRIMARY KEY,
            name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
            session_index INTEGER NOT NULL,
            start_us INTEGER NOT NULL,
            end_us INTEGER NOT NULL,
            created_at_utc TEXT NOT NULL,
            updated_at_utc TEXT NOT NULL,
            CHECK(start_us < end_us)
        );
        CREATE INDEX IF NOT EXISTS ranges_time ON ranges(start_us,end_us);
        CREATE INDEX IF NOT EXISTS ranges_name ON ranges(name COLLATE NOCASE);
        """
    )


def _metadata_set(connection: sqlite3.Connection, key: str, value: Any) -> None:
    connection.execute(
        "INSERT INTO metadata(key,value_json) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json",
        (key, json.dumps(value, ensure_ascii=False, separators=(",", ":"))),
    )


def _metadata_all(connection: sqlite3.Connection) -> dict[str, Any]:
    return {row["key"]: json.loads(row["value_json"]) for row in connection.execute("SELECT key,value_json FROM metadata")}


def _validate_database(connection: sqlite3.Connection) -> None:
    app_id = int(connection.execute("PRAGMA application_id").fetchone()[0])
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if app_id != APPLICATION_ID or version != USER_VERSION:
        raise ValueError("capture is not a supported FSTL Simpit Lab container")


def _encode_packet(relative_us: int, monotonic_us: int, observed_utc: str, datagram: bytes) -> bytes:
    utc_bytes = observed_utc.encode("utf-8")
    if len(utc_bytes) > 0xFFFF:
        raise ValueError("capture timestamp is too long")
    return _PACKET_PREFIX.pack(relative_us, monotonic_us, len(utc_bytes), len(datagram)) + utc_bytes + datagram


def _decode_packets(payload: bytes) -> Iterable[dict[str, Any]]:
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < _PACKET_PREFIX.size:
            raise ValueError("truncated capture packet prefix")
        relative_us, monotonic_us, utc_size, datagram_size = _PACKET_PREFIX.unpack_from(payload, offset)
        offset += _PACKET_PREFIX.size
        end = offset + utc_size + datagram_size
        if end > len(payload):
            raise ValueError("truncated capture packet")
        try:
            observed_utc = payload[offset : offset + utc_size].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError("invalid capture timestamp") from exc
        offset += utc_size
        datagram = payload[offset : offset + datagram_size]
        offset += datagram_size
        yield {
            "kind": "datagram",
            "timelineUs": int(relative_us),
            "receivedMonotonicUs": int(monotonic_us),
            "observedAtUtc": observed_utc,
            "datagram": datagram,
        }


class CaptureReader:
    """Indexed, bounded-memory reader for native and legacy captures."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.header: dict[str, Any]
        self.packet_count = 0
        self.duration_us = 0
        self._connection: sqlite3.Connection | None = None
        self._legacy_stream = None
        self._legacy_offsets = array("Q")
        self._legacy_timeline = array("Q")
        self._chunks: list[tuple[int, int, int, int]] = []
        self._chunk_ends = array("Q")
        self._chunk_last_times = array("Q")
        self._cached_chunk_index: int | None = None
        self._cached_chunk_packets: list[dict[str, Any]] = []

        with path.open("rb") as stream:
            sqlite_capture = stream.read(16).startswith(b"SQLite format 3\x00")
        if sqlite_capture:
            self._open_sqlite()
        else:
            self._open_legacy()

    def _open_sqlite(self) -> None:
        connection = _connect(self.path, writable=False)
        try:
            _validate_database(connection)
            self.header = {"kind": "header", **_metadata_all(connection)}
            cumulative = 0
            for row in connection.execute(
                "SELECT chunk_id,first_timeline_us,last_timeline_us,packet_count "
                "FROM chunks ORDER BY chunk_id"
            ):
                count = int(row["packet_count"])
                if count <= 0:
                    raise ValueError("capture chunk has no packets")
                cumulative += count
                self._chunks.append(
                    (
                        int(row["chunk_id"]),
                        int(row["first_timeline_us"]),
                        int(row["last_timeline_us"]),
                        count,
                    )
                )
                self._chunk_ends.append(cumulative)
                self._chunk_last_times.append(int(row["last_timeline_us"]))
            self.packet_count = cumulative
            self.duration_us = int(self.header.get("durationUs", 0))
            if self._chunks:
                self.duration_us = max(self.duration_us, self._chunks[-1][2])
            self._connection = connection
        except Exception:
            connection.close()
            raise

    def _open_legacy(self) -> None:
        stream = self.path.open("rb")
        try:
            first = stream.readline()
            if not first:
                raise ValueError("empty capture")
            try:
                header = json.loads(first.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise ValueError("invalid legacy capture header") from exc
            if header.get("schema") not in LEGACY_CAPTURE_SCHEMAS or header.get("kind") != "header":
                raise ValueError("capture header is not a supported FSTL dashboard capture")
            self.header = header
            first_monotonic: int | None = None
            while True:
                offset = stream.tell()
                line = stream.readline()
                if not line:
                    break
                try:
                    item = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise ValueError("invalid legacy capture item") from exc
                if item.get("kind") != "datagram":
                    raise ValueError("invalid legacy capture item")
                monotonic = int(item["receivedMonotonicUs"])
                if first_monotonic is None:
                    first_monotonic = monotonic
                timeline_us = max(0, monotonic - first_monotonic)
                if self._legacy_timeline and timeline_us < self._legacy_timeline[-1]:
                    raise ValueError("legacy capture timeline is not monotonic")
                self._legacy_offsets.append(offset)
                self._legacy_timeline.append(timeline_us)
            self.packet_count = len(self._legacy_offsets)
            self.duration_us = int(self._legacy_timeline[-1]) if self._legacy_timeline else 0
            self._legacy_stream = stream
        except Exception:
            stream.close()
            raise

    def close(self) -> None:
        if self._connection is not None:
            self._connection.close()
            self._connection = None
        if self._legacy_stream is not None:
            self._legacy_stream.close()
            self._legacy_stream = None
        self._cached_chunk_packets.clear()
        self._cached_chunk_index = None

    def __enter__(self) -> "CaptureReader":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _load_chunk(self, chunk_index: int) -> list[dict[str, Any]]:
        if self._cached_chunk_index == chunk_index:
            return self._cached_chunk_packets
        connection = self._connection
        if connection is None:
            raise ValueError("capture reader is closed")
        chunk_id, first_timeline_us, last_timeline_us, expected_count = self._chunks[chunk_index]
        row = connection.execute(
            "SELECT payload,sha256 FROM chunks WHERE chunk_id=?", (chunk_id,)
        ).fetchone()
        if row is None:
            raise ValueError("capture chunk is missing")
        try:
            raw = _decompress(bytes(row["payload"]))
        except Exception as exc:
            raise ValueError("capture chunk decompression failed") from exc
        if hashlib.sha256(raw).hexdigest() != row["sha256"]:
            raise ValueError("capture chunk checksum mismatch")
        packets = list(_decode_packets(raw))
        if (
            len(packets) != expected_count
            or not packets
            or int(packets[0]["timelineUs"]) != first_timeline_us
            or int(packets[-1]["timelineUs"]) != last_timeline_us
        ):
            raise ValueError("capture chunk index mismatch")
        self._cached_chunk_index = chunk_index
        self._cached_chunk_packets = packets
        return packets

    def packet(self, index: int) -> dict[str, Any]:
        if index < 0 or index >= self.packet_count:
            raise IndexError("capture packet index out of range")
        if self._legacy_stream is not None:
            self._legacy_stream.seek(self._legacy_offsets[index])
            item = json.loads(self._legacy_stream.readline().decode("utf-8"))
            return {
                **item,
                "timelineUs": int(self._legacy_timeline[index]),
                "datagram": base64.b64decode(item["datagramBase64"], validate=True),
                "receivedMonotonicUs": int(item["receivedMonotonicUs"]),
            }
        chunk_index = bisect.bisect_right(self._chunk_ends, index)
        chunk_start = 0 if chunk_index == 0 else int(self._chunk_ends[chunk_index - 1])
        return self._load_chunk(chunk_index)[index - chunk_start]

    def timeline_at(self, index: int) -> int:
        if self._legacy_stream is not None:
            if index < 0 or index >= self.packet_count:
                raise IndexError("capture packet index out of range")
            return int(self._legacy_timeline[index])
        return int(self.packet(index)["timelineUs"])

    def index_at_time(self, timeline_us: int) -> int:
        """Return the number of packets whose timeline is not newer than the target."""
        if timeline_us < 0 or self.packet_count == 0:
            return 0
        if self._legacy_stream is not None:
            return bisect.bisect_right(self._legacy_timeline, timeline_us)
        completed_chunks = bisect.bisect_right(self._chunk_last_times, timeline_us)
        if completed_chunks >= len(self._chunks):
            return self.packet_count
        start = 0 if completed_chunks == 0 else int(self._chunk_ends[completed_chunks - 1])
        packets = self._load_chunk(completed_chunks)
        local_timeline = [int(packet["timelineUs"]) for packet in packets]
        return start + bisect.bisect_right(local_timeline, timeline_us)

    def packets(self, start: int = 0, end: int | None = None) -> Iterable[dict[str, Any]]:
        upper = self.packet_count if end is None else min(self.packet_count, end)
        for index in range(max(0, start), upper):
            yield self.packet(index)


def open_capture(path: Path) -> CaptureReader:
    try:
        return CaptureReader(path)
    except sqlite3.DatabaseError as exc:
        raise ValueError("invalid SQLite capture") from exc


class CaptureWriter:
    def __init__(
        self,
        capture_dir: Path,
        *,
        warn_bytes: int = DEFAULT_WARN_BYTES,
        stop_bytes: int = DEFAULT_STOP_BYTES,
        free_reserve_bytes: int = DEFAULT_FREE_RESERVE_BYTES,
    ) -> None:
        self.capture_dir = capture_dir
        self.capture_dir.mkdir(parents=True, exist_ok=True)
        self.warn_bytes = warn_bytes
        self.stop_bytes = stop_bytes
        self.free_reserve_bytes = free_reserve_bytes
        self.path: Path | None = None
        self.capture_id: str | None = None
        self._connection: sqlite3.Connection | None = None
        self._lock = threading.RLock()
        self._buffer = bytearray()
        self._buffer_first_us: int | None = None
        self._buffer_last_us: int = 0
        self._buffer_packets = 0
        self._first_monotonic_us: int | None = None
        self._last_relative_us = 0
        self._packet_count = 0
        self._recent_bytes: deque[tuple[float, int]] = deque()
        self._started_wall_time: float | None = None
        self._last_size_sample = 0
        self._expected_duration_us: int | None = None
        self.stop_reason: str | None = None

    @property
    def active(self) -> bool:
        return self._connection is not None

    def start(self, metadata: dict[str, Any]) -> Path:
        with self._lock:
            self.stop()
            stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
            self.capture_id = str(uuid.uuid4())
            safe_name = "".join(character if character.isalnum() or character in "-_" else "-" for character in str(metadata.get("name") or "telemetry")).strip("-")[:64] or "telemetry"
            self.path = self.capture_dir / f"{stamp}-{safe_name}-{self.capture_id[:8]}.fstlcap"
            self._connection = _connect(self.path, writable=True)
            _create_schema(self._connection)
            created = _utc_now()
            base = {
                "schema": CAPTURE_SCHEMA,
                "captureId": self.capture_id,
                "name": metadata.get("name") or f"Capture {stamp}",
                "createdAtUtc": created,
                "closedAtUtc": None,
                "complete": False,
                "stopReason": None,
                "durationUs": 0,
                "packetCount": 0,
                **metadata,
            }
            self._expected_duration_us = int(metadata["expectedDurationUs"]) if metadata.get("expectedDurationUs") is not None else None
            for key, value in base.items():
                _metadata_set(self._connection, key, value)
            self._connection.commit()
            self._buffer.clear()
            self._buffer_first_us = None
            self._buffer_packets = 0
            self._first_monotonic_us = None
            self._last_relative_us = 0
            self._packet_count = 0
            self._recent_bytes.clear()
            self._started_wall_time = time.monotonic()
            self._last_size_sample = _database_size(self.path)
            self.stop_reason = None
            return self.path

    def _flush_chunk(self) -> None:
        connection = self._connection
        if connection is None or not self._buffer_packets or self._buffer_first_us is None:
            return
        raw = bytes(self._buffer)
        compressed = _compress(raw)
        connection.execute(
            "INSERT INTO chunks(first_timeline_us,last_timeline_us,packet_count,uncompressed_bytes,compressed_bytes,sha256,codec,payload) VALUES(?,?,?,?,?,?,?,?)",
            (self._buffer_first_us, self._buffer_last_us, self._buffer_packets, len(raw), len(compressed), hashlib.sha256(raw).hexdigest(), "zstd", compressed),
        )
        _metadata_set(connection, "durationUs", self._last_relative_us)
        _metadata_set(connection, "packetCount", self._packet_count)
        connection.commit()
        current_size = _database_size(self.path) if self.path else 0
        self._recent_bytes.append(
            (time.monotonic(), max(0, current_size - self._last_size_sample))
        )
        self._last_size_sample = current_size
        self._buffer.clear()
        self._buffer_first_us = None
        self._buffer_packets = 0

    def packet(self, datagram: bytes, monotonic_us: int, observed_at_utc: str) -> None:
        with self._lock:
            if self._connection is None:
                return
            if self._first_monotonic_us is None:
                self._first_monotonic_us = monotonic_us
            relative_us = max(0, monotonic_us - self._first_monotonic_us)
            encoded = _encode_packet(relative_us, monotonic_us, observed_at_utc, datagram)
            if self._buffer_first_us is None:
                self._buffer_first_us = relative_us
            self._buffer.extend(encoded)
            self._buffer_last_us = relative_us
            self._last_relative_us = relative_us
            self._buffer_packets += 1
            self._packet_count += 1
            if relative_us - self._buffer_first_us >= CHUNK_MAX_SPAN_US or len(self._buffer) >= CHUNK_MAX_BYTES:
                self._flush_chunk()
                self._enforce_limits()

    def _enforce_limits(self) -> None:
        if self.path is None or self._connection is None:
            return
        size = _database_size(self.path)
        free = shutil.disk_usage(self.path.parent).free
        reason = None
        if self.stop_bytes > 0 and size >= self.stop_bytes:
            reason = "size-limit"
        elif free < self.free_reserve_bytes:
            reason = "free-space-reserve"
        if reason is not None:
            self.stop(reason=reason)

    def checkpoint(self, session_index: int, timeline_us: int, state_blob: bytes) -> None:
        with self._lock:
            if self._connection is None:
                return
            compressed = _compress(state_blob)
            self._connection.execute(
                "INSERT OR REPLACE INTO checkpoints(session_index,timeline_us,state_blob,sha256) VALUES(?,?,?,?)",
                (session_index, timeline_us, compressed, hashlib.sha256(state_blob).hexdigest()),
            )
            self._connection.commit()

    def metadata(self, **values: Any) -> None:
        with self._lock:
            if self._connection is None:
                return
            for key, value in values.items():
                _metadata_set(self._connection, key, value)
            self._connection.commit()

    def session_boundary(self, session_index: int, session_id: int, timeline_us: int, reason: str) -> None:
        with self._lock:
            if self._connection is None:
                return
            self._connection.execute(
                "UPDATE sessions SET end_us=? WHERE end_us IS NULL",
                (timeline_us,),
            )
            self._connection.execute(
                "INSERT OR REPLACE INTO sessions(session_index,session_id,start_us,end_us,reason) VALUES(?,?,?,?,?)",
                (session_index, str(session_id), timeline_us, None, reason),
            )
            self._connection.commit()

    def metrics(self) -> dict[str, Any]:
        with self._lock:
            size = _database_size(self.path) if self.path else 0
            now = time.monotonic()
            while self._recent_bytes and now - self._recent_bytes[0][0] > 60.0:
                self._recent_bytes.popleft()
            window = max(
                1.0,
                min(60.0, now - self._started_wall_time),
            ) if self._started_wall_time is not None else 1.0
            bytes_per_second = sum(item[1] for item in self._recent_bytes) / window
            expected = None
            if self._expected_duration_us is not None and self._expected_duration_us > self._last_relative_us:
                expected = int(size + bytes_per_second * ((self._expected_duration_us - self._last_relative_us) / 1_000_000.0))
            free = shutil.disk_usage(self.capture_dir).free
            limiting_bytes = min(
                self.stop_bytes - size if self.stop_bytes > 0 else free,
                max(0, free - self.free_reserve_bytes),
            )
            return {
                "bytes": size,
                "durationUs": self._last_relative_us,
                "packetCount": self._packet_count,
                "bytesPerSecond": bytes_per_second,
                "rollingBytesPerSecond": bytes_per_second,
                "projected10MinutesBytes": int(size + bytes_per_second * 600),
                "projectedBytes10Minutes": int(size + bytes_per_second * 600),
                "projected1HourBytes": int(size + bytes_per_second * 3600),
                "projectedBytes1Hour": int(size + bytes_per_second * 3600),
                "estimatedFinalBytes": expected,
                "projectedFinalBytes": expected,
                "freeBytes": free,
                "secondsUntilLimit": limiting_bytes / bytes_per_second if bytes_per_second > 0 else None,
                "warning": "Seuil d’avertissement de taille atteint" if size >= self.warn_bytes else None,
                "warnBytes": self.warn_bytes,
                "stopBytes": self.stop_bytes,
                "freeReserveBytes": self.free_reserve_bytes,
                "stopReason": self.stop_reason,
            }

    def stop(self, *, reason: str = "user") -> Path | None:
        with self._lock:
            path = self.path
            connection = self._connection
            if connection is None:
                return path
            self._flush_chunk()
            self.stop_reason = reason
            _metadata_set(connection, "closedAtUtc", _utc_now())
            _metadata_set(connection, "complete", True)
            _metadata_set(connection, "stopReason", reason)
            _metadata_set(connection, "durationUs", self._last_relative_us)
            _metadata_set(connection, "packetCount", self._packet_count)
            connection.execute("UPDATE sessions SET end_us=? WHERE end_us IS NULL", (self._last_relative_us,))
            connection.commit()
            connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            connection.close()
            self._connection = None
            return path


def load_capture(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Compatibility helper for bounded tests and tools; replay uses ``CaptureReader``."""
    with open_capture(path) as reader:
        return dict(reader.header), list(reader.packets())


def load_checkpoint(path: Path, timeline_us: int) -> dict[str, Any] | None:
    """Return the newest verified state checkpoint not newer than ``timeline_us``."""
    with path.open("rb") as stream:
        if not stream.read(16).startswith(b"SQLite format 3\x00"):
            return None
    with closing(_connect(path, writable=False)) as connection:
        _validate_database(connection)
        row = connection.execute(
            "SELECT session_index,timeline_us,state_blob,sha256 FROM checkpoints "
            "WHERE timeline_us<=? ORDER BY timeline_us DESC LIMIT 1",
            (timeline_us,),
        ).fetchone()
        if row is None:
            return None
        try:
            raw = _decompress(bytes(row["state_blob"]))
        except Exception as exc:
            raise ValueError("capture checkpoint decompression failed") from exc
        if hashlib.sha256(raw).hexdigest() != row["sha256"]:
            raise ValueError("capture checkpoint checksum mismatch")
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("invalid capture checkpoint")
        return {
            "sessionIndex": int(row["session_index"]),
            "timelineUs": int(row["timeline_us"]),
            "state": value,
        }


class CaptureLibrary:
    def __init__(self, root: Path, *, free_reserve_bytes: int = DEFAULT_FREE_RESERVE_BYTES) -> None:
        self.root = root
        self.free_reserve_bytes = free_reserve_bytes
        self.root.mkdir(parents=True, exist_ok=True)

    def _describe(self, path: Path) -> dict[str, Any]:
        if path.suffix == ".fstlcap":
            with closing(_connect(path, writable=False)) as connection:
                _validate_database(connection)
                metadata = _metadata_all(connection)
                session_count = int(connection.execute("SELECT count(*) FROM sessions").fetchone()[0])
                range_count = int(connection.execute("SELECT count(*) FROM ranges").fetchone()[0])
            capture_id = str(metadata.get("captureId"))
            size = _database_size(path)
            return {**metadata, "id": capture_id, "path": str(path), "bytes": size, "sizeBytes": size, "sessionCount": session_count, "rangeCount": range_count, "legacy": False}
        with open_capture(path) as reader:
            size = path.stat().st_size
            return {**reader.header, "id": f"legacy:{hashlib.sha256(str(path).encode()).hexdigest()[:16]}", "path": str(path), "name": path.stem, "bytes": size, "sizeBytes": size, "durationUs": reader.duration_us, "packetCount": reader.packet_count, "sessionCount": 0, "rangeCount": 0, "legacy": True, "complete": True}

    def list(self, query: str = "") -> list[dict[str, Any]]:
        needle = query.casefold().strip()
        result = []
        for path in sorted((*self.root.glob("*.fstlcap"), *self.root.glob("*.fstlcap.jsonl")), key=lambda item: item.stat().st_mtime, reverse=True):
            try:
                item = self._describe(path)
            except (OSError, ValueError, sqlite3.DatabaseError, json.JSONDecodeError):
                continue
            if not needle or needle in str(item.get("name", "")).casefold():
                result.append(item)
        return result

    def resolve(self, capture_id: str) -> Path:
        for item in self.list():
            if item["id"] == capture_id:
                return Path(item["path"])
        raise ValueError("capture not found")

    def rename(self, capture_id: str, name: str) -> dict[str, Any]:
        normalized = name.strip()
        if not 1 <= len(normalized) <= 128:
            raise ValueError("capture name must contain 1..128 characters")
        path = self.resolve(capture_id)
        if path.suffix != ".fstlcap":
            raise ValueError("legacy capture must be imported before editing")
        with closing(_connect(path, writable=True)) as connection:
            _validate_database(connection)
            _metadata_set(connection, "name", normalized)
            connection.commit()
        return self._describe(path)

    def delete(self, capture_id: str) -> None:
        path = self.resolve(capture_id).resolve()
        if self.root.resolve() not in path.parents:
            raise ValueError("capture outside library")
        path.unlink()
        for suffix in ("-wal", "-shm"):
            Path(str(path) + suffix).unlink(missing_ok=True)

    def import_path(self, source: Path) -> dict[str, Any]:
        with source.open("rb") as stream:
            native = stream.read(16).startswith(b"SQLite format 3\x00")
        if native:
            return self._import_native(source)

        with open_capture(source) as reader:
            writer = CaptureWriter(self.root)
            path = writer.start({"name": source.stem, "importedFromSchema": reader.header.get("schema"), "contractFeatures": reader.header.get("contractFeatures", [])})
            session_index = -1
            last_session = 0
            try:
                for packet in reader.packets():
                    writer.packet(packet["datagram"], int(packet["receivedMonotonicUs"]), packet["observedAtUtc"])
                    datagram = packet["datagram"]
                    if (
                        len(datagram) >= 68
                        and int.from_bytes(datagram[0:4], "little") == 0x4C545346
                        and datagram[6] == 3
                    ):
                        session_id = int.from_bytes(datagram[12:20], "little")
                        if session_id and session_id != last_session:
                            session_index += 1
                            last_session = session_id
                            writer.session_boundary(
                                session_index,
                                session_id,
                                int(packet.get("timelineUs", 0)),
                                "imported-welcome",
                            )
                writer.stop(reason="imported")
            except Exception:
                writer.stop(reason="import-failed")
                raise
        return self._describe(path)

    def _import_native(self, source: Path) -> dict[str, Any]:
        with closing(_connect(source, writable=False)) as source_connection:
            _validate_database(source_connection)
            metadata = _metadata_all(source_connection)
            page_count = int(source_connection.execute("PRAGMA page_count").fetchone()[0])
            page_size = int(source_connection.execute("PRAGMA page_size").fetchone()[0])
            required_copy_bytes = page_count * page_size
            free_bytes = shutil.disk_usage(self.root).free
            if free_bytes - required_copy_bytes < self.free_reserve_bytes:
                raise OSError(
                    errno.ENOSPC,
                    "native capture import would exceed the free-space reserve",
                )
            old_capture_id = str(metadata.get("captureId", ""))
            new_capture_id = str(uuid.uuid4())
            stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
            safe_name = "".join(
                character if character.isalnum() or character in "-_" else "-"
                for character in str(metadata.get("name") or source.stem)
            ).strip("-")[:64] or "capture"
            destination = self.root / f"{stamp}-{safe_name}-{new_capture_id[:8]}.fstlcap"
            try:
                with closing(_connect(destination, writable=True)) as destination_connection:
                    source_connection.backup(destination_connection)
                    _validate_database(destination_connection)
                    _metadata_set(destination_connection, "captureId", new_capture_id)
                    _metadata_set(destination_connection, "importedAtUtc", _utc_now())
                    if old_capture_id:
                        _metadata_set(destination_connection, "importedFromCaptureId", old_capture_id)
                    destination_connection.commit()
                    destination_connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            except Exception:
                for suffix in ("", "-wal", "-shm"):
                    Path(str(destination) + suffix).unlink(missing_ok=True)
                raise
        return self._describe(destination)

    def ranges(self, capture_id: str, query: str = "") -> list[dict[str, Any]]:
        path = self.resolve(capture_id)
        if path.suffix != ".fstlcap":
            return []
        with closing(_connect(path, writable=False)) as connection:
            _validate_database(connection)
            rows = connection.execute(
                "SELECT * FROM ranges WHERE lower(name) LIKE lower(?) ORDER BY start_us,range_id",
                (f"%{query.strip()}%",),
            )
            return [{"id": row["range_id"], "name": row["name"], "sessionIndex": row["session_index"], "startUs": row["start_us"], "endUs": row["end_us"]} for row in rows]

    def sessions(self, capture_id: str) -> list[dict[str, Any]]:
        path = self.resolve(capture_id)
        if path.suffix != ".fstlcap":
            return []
        with closing(_connect(path, writable=False)) as connection:
            _validate_database(connection)
            return [
                {
                    "sessionIndex": row["session_index"],
                    "sessionId": row["session_id"],
                    "startUs": row["start_us"],
                    "endUs": row["end_us"],
                    "reason": row["reason"],
                }
                for row in connection.execute("SELECT * FROM sessions ORDER BY session_index")
            ]

    def _session_for_range(self, connection: sqlite3.Connection, start_us: int, end_us: int) -> int:
        row = connection.execute(
            "SELECT session_index FROM sessions WHERE start_us<=? AND coalesce(end_us,9223372036854775807)>=? ORDER BY session_index LIMIT 1",
            (start_us, end_us),
        ).fetchone()
        if row is None:
            session_count = int(connection.execute("SELECT count(*) FROM sessions").fetchone()[0])
            if session_count == 0:
                return 0
            raise ValueError("range must remain inside one recorded session")
        return int(row[0])

    def create_range(self, capture_id: str, name: str, start_us: int, end_us: int) -> dict[str, Any]:
        normalized = name.strip()
        if not 1 <= len(normalized) <= 128 or start_us < 0 or end_us <= start_us:
            raise ValueError("invalid capture range")
        path = self.resolve(capture_id)
        if path.suffix != ".fstlcap":
            raise ValueError("legacy capture must be imported before adding ranges")
        with closing(_connect(path, writable=True)) as connection:
            _validate_database(connection)
            session = self._session_for_range(connection, start_us, end_us)
            range_id = str(uuid.uuid4())
            now = _utc_now()
            connection.execute("INSERT INTO ranges VALUES(?,?,?,?,?,?,?)", (range_id, normalized, session, start_us, end_us, now, now))
            connection.commit()
        return {"id": range_id, "name": normalized, "sessionIndex": session, "startUs": start_us, "endUs": end_us}

    def update_range(self, capture_id: str, range_id: str, *, name: str, start_us: int, end_us: int) -> dict[str, Any]:
        path = self.resolve(capture_id)
        normalized = name.strip()
        if not 1 <= len(normalized) <= 128 or start_us < 0 or end_us <= start_us:
            raise ValueError("invalid capture range")
        with closing(_connect(path, writable=True)) as connection:
            _validate_database(connection)
            session = self._session_for_range(connection, start_us, end_us)
            cursor = connection.execute("UPDATE ranges SET name=?,session_index=?,start_us=?,end_us=?,updated_at_utc=? WHERE range_id=?", (normalized, session, start_us, end_us, _utc_now(), range_id))
            if cursor.rowcount != 1:
                raise ValueError("capture range not found")
            connection.commit()
        return {"id": range_id, "name": normalized, "sessionIndex": session, "startUs": start_us, "endUs": end_us}

    def delete_range(self, capture_id: str, range_id: str) -> None:
        path = self.resolve(capture_id)
        with closing(_connect(path, writable=True)) as connection:
            _validate_database(connection)
            cursor = connection.execute("DELETE FROM ranges WHERE range_id=?", (range_id,))
            if cursor.rowcount != 1:
                raise ValueError("capture range not found")
            connection.commit()

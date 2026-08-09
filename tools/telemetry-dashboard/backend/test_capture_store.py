from __future__ import annotations

import base64
import errno
import json
import sqlite3
import tempfile
import unittest
from contextlib import closing
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from capture_store import (
    CAPTURE_SCHEMA,
    LEGACY_CAPTURE_SCHEMAS,
    CaptureLibrary,
    CaptureWriter,
    load_capture,
    load_checkpoint,
    open_capture,
)


class CaptureStoreTest(unittest.TestCase):
    def test_chunked_round_trip_checkpoint_and_library_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = CaptureWriter(root)
            path = writer.start({"name": "Long patrol", "expectedDurationUs": 60_000_000})
            writer.session_boundary(0, 42, 0, "welcome")
            writer.packet(b"first", 10, "2026-08-09T10:00:00.000010Z")
            writer.packet(b"second", 1_000_011, "2026-08-09T10:00:01.000011Z")
            writer.checkpoint(0, 1_000_001, b'{"state":"ok"}')
            writer.stop()

            header, packets = load_capture(path)
            self.assertEqual(CAPTURE_SCHEMA, header["schema"])
            self.assertEqual([b"first", b"second"], [item["datagram"] for item in packets])
            self.assertEqual("ok", load_checkpoint(path, 2_000_000)["state"]["state"])
            item = CaptureLibrary(root).list()[0]
            self.assertEqual("Long patrol", item["name"])
            self.assertGreater(item["sizeBytes"], 0)
            self.assertEqual(1, item["sessionCount"])
            self.assertTrue(item["complete"])

    def test_checksum_corruption_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            writer = CaptureWriter(Path(directory))
            path = writer.start({"name": "corrupt"})
            writer.packet(b"payload", 1, "2026-08-09T10:00:00.000001Z")
            writer.stop()
            with closing(sqlite3.connect(path)) as connection:
                connection.execute("UPDATE chunks SET sha256=?", ("0" * 64,))
                connection.commit()
            with self.assertRaisesRegex(ValueError, "checksum"):
                load_capture(path)

    def test_ranges_are_searchable_and_cannot_cross_session_boundaries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = CaptureWriter(root)
            path = writer.start({"name": "ranges"})
            writer.packet(b"a", 0, "2026-08-09T10:00:00.000000Z")
            writer.session_boundary(0, 100, 0, "welcome")
            writer.packet(b"b", 10_000_000, "2026-08-09T10:00:10.000000Z")
            writer.session_boundary(1, 200, 10_000_000, "welcome")
            writer.packet(b"c", 20_000_000, "2026-08-09T10:00:20.000000Z")
            writer.stop()
            library = CaptureLibrary(root)
            capture_id = library.list()[0]["id"]
            marker = library.create_range(capture_id, "Missile launch", 1_000_000, 2_000_000)
            self.assertEqual(marker["id"], library.ranges(capture_id, "LAUNCH")[0]["id"])
            with self.assertRaisesRegex(ValueError, "one recorded session"):
                library.create_range(capture_id, "cross", 9_000_000, 11_000_000)
            self.assertEqual(path, library.resolve(capture_id))

    def test_explicit_legacy_import_converts_to_sqlite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = root / "legacy-source.jsonl"
            legacy.write_text(
                "\n".join([
                    json.dumps({"schema": LEGACY_CAPTURE_SCHEMAS[1], "kind": "header"}),
                    json.dumps({
                        "kind": "datagram",
                        "receivedMonotonicUs": "25",
                        "observedAtUtc": "1970-01-01T00:00:00.000025Z",
                        "datagramBase64": base64.b64encode(b"fstl").decode("ascii"),
                    }),
                ]) + "\n",
                encoding="utf-8",
            )
            library_root = root / "library"
            imported = CaptureLibrary(library_root).import_path(legacy)
            self.assertFalse(imported["legacy"])
            self.assertEqual(CAPTURE_SCHEMA, imported["schema"])
            self.assertEqual(b"fstl", load_capture(Path(imported["path"]))[1][0]["datagram"])

    def test_native_import_preserves_ranges_checkpoints_sessions_and_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root = root / "source"
            library_root = root / "library"
            writer = CaptureWriter(source_root)
            source = writer.start({"name": "Marked patrol", "mission": "sm2-01"})
            writer.session_boundary(0, 42, 0, "welcome")
            writer.packet(b"first", 10, "2026-08-09T10:00:00.000010Z")
            writer.packet(b"second", 1_000_011, "2026-08-09T10:00:01.000011Z")
            writer.checkpoint(0, 1_000_001, b'{"state":"preserved"}')
            writer.stop()
            source_library = CaptureLibrary(source_root)
            source_id = source_library.list()[0]["id"]
            marker = source_library.create_range(source_id, "Missile launch", 1, 100)

            imported = CaptureLibrary(library_root, free_reserve_bytes=0).import_path(source)

            self.assertNotEqual(source_id, imported["id"])
            self.assertEqual("Marked patrol", imported["name"])
            self.assertEqual("sm2-01", imported["mission"])
            self.assertEqual(1, imported["sessionCount"])
            self.assertEqual(1, imported["rangeCount"])
            imported_library = CaptureLibrary(library_root)
            self.assertEqual(marker["name"], imported_library.ranges(imported["id"])[0]["name"])
            checkpoint = load_checkpoint(Path(imported["path"]), 2_000_000)
            self.assertIsNotNone(checkpoint)
            self.assertEqual("preserved", checkpoint["state"]["state"])

    def test_native_import_preserves_configured_free_space_reserve(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root = root / "source"
            library_root = root / "library"
            writer = CaptureWriter(source_root)
            source = writer.start({"name": "large import"})
            writer.packet(b"payload", 1, "2026-08-09T10:00:00.000001Z")
            writer.stop()
            with closing(sqlite3.connect(source)) as connection:
                logical_bytes = (
                    int(connection.execute("PRAGMA page_count").fetchone()[0])
                    * int(connection.execute("PRAGMA page_size").fetchone()[0])
                )
            reserve = 10_000
            library = CaptureLibrary(library_root, free_reserve_bytes=reserve)

            with mock.patch(
                "capture_store.shutil.disk_usage",
                return_value=SimpleNamespace(free=logical_bytes + reserve - 1),
            ):
                with self.assertRaises(OSError) as raised:
                    library.import_path(source)

            self.assertEqual(errno.ENOSPC, raised.exception.errno)
            self.assertEqual([], list(library_root.glob("*.fstlcap")))

    def test_delete_removes_wal_and_shared_memory_sidecars(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = CaptureWriter(root)
            path = writer.start({"name": "delete sidecars"})
            writer.packet(b"payload", 1, "2026-08-09T10:00:00.000001Z")
            writer.stop()
            library = CaptureLibrary(root)
            capture_id = library.list()[0]["id"]
            sidecars = [Path(str(path) + suffix) for suffix in ("-wal", "-shm")]
            for sidecar in sidecars:
                sidecar.write_bytes(b"orphaned sqlite sidecar")

            with mock.patch.object(library, "resolve", return_value=path):
                library.delete(capture_id)

            self.assertFalse(path.exists())
            self.assertTrue(all(not sidecar.exists() for sidecar in sidecars))

    def test_indexed_reader_loads_only_the_addressed_chunk(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            writer = CaptureWriter(Path(directory))
            path = writer.start({"name": "indexed"})
            writer.packet(b"first", 0, "2026-08-09T10:00:00.000000Z")
            writer.packet(b"second", 1_000_001, "2026-08-09T10:00:01.000001Z")
            writer.packet(b"third", 2_000_002, "2026-08-09T10:00:02.000002Z")
            writer.stop()

            with open_capture(path) as reader:
                self.assertEqual(3, reader.packet_count)
                self.assertEqual(2, reader.index_at_time(1_500_000))
                self.assertEqual(b"third", reader.packet(2)["datagram"])
                self.assertEqual(1, len(reader._cached_chunk_packets))

    def test_size_limit_finalizes_automatically_after_a_validated_chunk(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            writer = CaptureWriter(
                Path(directory), warn_bytes=1, stop_bytes=1, free_reserve_bytes=0
            )
            path = writer.start({"name": "bounded"})
            writer.packet(b"first", 0, "2026-08-09T10:00:00.000000Z")
            writer.packet(b"second", 1_000_001, "2026-08-09T10:00:01.000001Z")
            self.assertFalse(writer.active)
            self.assertEqual("size-limit", writer.stop_reason)
            header, packets = load_capture(path)
            self.assertTrue(header["complete"])
            self.assertEqual(2, len(packets))

    def test_interrupted_capture_recovers_through_last_committed_chunk(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = CaptureWriter(root)
            path = writer.start({"name": "interrupted"})
            writer.packet(b"kept-a", 0, "2026-08-09T10:00:00.000000Z")
            writer.packet(b"kept-b", 1_000_001, "2026-08-09T10:00:01.000001Z")
            assert writer._connection is not None
            writer._connection.close()
            writer._connection = None
            item = CaptureLibrary(root).list()[0]
            self.assertFalse(item["complete"])
            self.assertEqual([b"kept-a", b"kept-b"], [packet["datagram"] for packet in load_capture(path)[1]])


if __name__ == "__main__":
    unittest.main()

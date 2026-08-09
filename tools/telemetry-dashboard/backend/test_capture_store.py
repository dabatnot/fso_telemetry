from __future__ import annotations

import base64
import json
import sqlite3
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

from capture_store import (
    CAPTURE_SCHEMA,
    LEGACY_CAPTURE_SCHEMAS,
    CaptureLibrary,
    CaptureWriter,
    load_capture,
    load_checkpoint,
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

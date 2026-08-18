from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.config import ConfigStore  # noqa: E402
from av_core.models import AvCoreConfig  # noqa: E402
from av_core.runtime import AvCoreRuntime  # noqa: E402
from pydantic import ValidationError  # noqa: E402


class ConfigModelTest(unittest.TestCase):
    def test_defaults_match_the_phase_one_contract(self) -> None:
        payload = AvCoreConfig().model_dump(mode="json", by_alias=True)
        self.assertEqual(1, payload["schemaVersion"])
        self.assertEqual("127.0.0.1", payload["telemetry"]["host"])
        self.assertEqual(42042, payload["telemetry"]["port"])
        self.assertEqual(8080, payload["web"]["port"])
        self.assertTrue(payload["modules"]["warnCtrl"]["installed"])
        self.assertFalse(payload["modules"]["sensProc"]["installed"])
        self.assertEqual(30, payload["lighting"]["maxBrightnessPercent"])
        self.assertNotIn("nightBrightnessPercent", payload["lighting"])

    def test_unknown_fields_and_invalid_ranges_are_rejected(self) -> None:
        with self.assertRaises(ValidationError):
            AvCoreConfig.model_validate({"schemaVersion": 1, "unknown": True})
        with self.assertRaises(ValidationError):
            AvCoreConfig.model_validate(
                {
                    "telemetry": {"host": "", "port": 0, "staleAfterMs": 60001},
                }
            )

    def test_hysteresis_and_flash_order_are_strict(self) -> None:
        base = AvCoreConfig().model_dump(mode="json", by_alias=True)
        base["alerts"]["engine"] = {
            "activateBelowPercent": 50,
            "clearAbovePercent": 50,
        }
        with self.assertRaises(ValidationError):
            AvCoreConfig.model_validate(base)

        base = AvCoreConfig().model_dump(mode="json", by_alias=True)
        base["lighting"]["slowFlashHz"] = 4
        base["lighting"]["fastFlashHz"] = 4
        with self.assertRaises(ValidationError):
            AvCoreConfig.model_validate(base)


class ConfigStoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "av-core.json"
        self.store = ConfigStore(self.path)

    def test_missing_file_is_created_with_defaults(self) -> None:
        loaded = self.store.load()
        self.assertIsNone(loaded.error)
        self.assertTrue(self.path.is_file())
        self.assertEqual(8080, json.loads(self.path.read_text(encoding="utf-8"))["web"]["port"])

    def test_valid_file_is_loaded(self) -> None:
        payload = AvCoreConfig().model_dump(mode="json", by_alias=True)
        payload["web"]["port"] = 9090
        self.path.write_text(json.dumps(payload), encoding="utf-8")
        loaded = self.store.load()
        self.assertIsNone(loaded.error)
        self.assertEqual(9090, loaded.config.web.port)

    def test_corrupt_file_is_preserved_and_defaults_are_used(self) -> None:
        self.path.write_text("{not-json", encoding="utf-8")
        loaded = self.store.load()
        self.assertIsNotNone(loaded.error)
        self.assertEqual("{not-json", self.path.read_text(encoding="utf-8"))
        self.assertEqual(8080, loaded.config.web.port)

    def test_failed_replace_preserves_disk_and_runtime_state(self) -> None:
        original = AvCoreConfig()
        self.store.save(original)
        runtime = AvCoreRuntime(self.store)
        updated = original.model_copy(deep=True)
        updated.web.port = 9090

        with patch("av_core.config.os.replace", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError):
                runtime.update_config(updated)

        self.assertEqual(8080, runtime.config.web.port)
        self.assertEqual(8080, json.loads(self.path.read_text(encoding="utf-8"))["web"]["port"])
        self.assertEqual([], list(self.path.parent.glob("*.tmp")))


if __name__ == "__main__":
    unittest.main()

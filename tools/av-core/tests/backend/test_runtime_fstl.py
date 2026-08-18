from __future__ import annotations

import sys
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.config import ConfigStore  # noqa: E402
from av_core.fstl_service import FstlFrame  # noqa: E402
from av_core.runtime import AvCoreRuntime  # noqa: E402


class FakeFstlService:
    def __init__(self, config, callback):
        self.config = config
        self.callback = callback
        self.started = False
        self.stopped = False
        self.reconfigurations = []

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def reconfigure(self, config):
        self.config = config
        self.reconfigurations.append(config)

    def emit(self, frame):
        self.callback(frame)


class RuntimeFstlTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.runtime = AvCoreRuntime(
            ConfigStore(Path(self.directory.name) / "av-core.json"),
            fstl_factory=FakeFstlService,
        )
        self.service: FakeFstlService = self.runtime._fstl  # type: ignore[assignment]

    @staticmethod
    def live_frame(engine_ratio: float = 0.49) -> FstlFrame:
        records = {
            "FLIGHT_STATE": [{"entity_id": "1"}],
            "HUD_ALERT_STATE": [{
                "entity_id": "1", "presence": 0, "primary_fire_threat_active": True,
                "missile_lock_state": 0,
            }],
            "THREAT_STATE": [{"entity_id": "1", "incoming_missiles": []}],
        }
        return FstlFrame(
            state="LIVE", session_id="42", last_live_monotonic=1.0,
            player_entity_id="1", records=records,
            derived={
                "entities.1.engine_integrity_ratio": {
                    "available": True, "reason": None, "value": engine_ratio,
                }
            },
        )

    def test_lifecycle_and_alerts_follow_fstl_frames(self) -> None:
        self.assertEqual("DISCONNECTED", self.runtime.status().telemetry.state)
        self.runtime.start()
        self.assertTrue(self.service.started)
        self.service.emit(self.live_frame())
        live = self.runtime.status()
        self.assertEqual("LIVE", live.telemetry.state)
        self.assertEqual("42", live.telemetry.session_id)
        self.assertTrue(live.cockpit.available)
        self.assertTrue(live.cockpit.warnings.fire)
        self.assertEqual("ACTIVE", live.cockpit.cautions.engine.state)

        self.service.emit(FstlFrame(state="STALE", session_id="42", last_live_monotonic=1.0))
        stale = self.runtime.status()
        self.assertEqual("STALE", stale.telemetry.state)
        self.assertFalse(stale.cockpit.available)
        self.assertFalse(stale.cockpit.warnings.master)
        self.service.emit(self.live_frame(engine_ratio=0.75))
        resumed = self.runtime.status()
        self.assertEqual("LIVE", resumed.telemetry.state)
        self.assertTrue(resumed.cockpit.available)
        self.assertEqual("CLEAR", resumed.cockpit.cautions.engine.state)
        self.runtime.stop()
        self.assertTrue(self.service.stopped)

    def test_threshold_change_recalculates_without_fstl_reconnect(self) -> None:
        self.service.emit(self.live_frame(engine_ratio=0.45))
        config = self.runtime.config
        config.alerts.engine.activate_below_percent = 40
        config.alerts.engine.clear_above_percent = 50
        self.runtime.update_config(config)
        self.assertEqual("CLEAR", self.runtime.status().cockpit.cautions.engine.state)
        self.assertEqual([], self.service.reconfigurations)

    def test_endpoint_change_requests_one_reconfiguration(self) -> None:
        config = self.runtime.config
        config.telemetry.port = 42043
        self.runtime.update_config(config)
        self.assertEqual(1, len(self.service.reconfigurations))
        self.assertEqual(42043, self.service.reconfigurations[0].port)


class InstalledFstlImportTest(unittest.TestCase):
    def test_fstl_client_imports_from_an_isolated_install_directory(self) -> None:
        source = Path(__file__).resolve().parents[4] / "test" / "telemetry" / "protocol" / "tools"
        with tempfile.TemporaryDirectory() as directory:
            install = Path(directory) / "fstl-client"
            install.mkdir()
            for name in ("fstl_client_core.py", "fstl_reference_decoder.py"):
                shutil.copy2(source / name, install / name)
            environment = os.environ.copy()
            environment["AV_CORE_FSTL_CLIENT_DIR"] = str(install)
            environment["PYTHONPATH"] = str(BACKEND_ROOT)
            completed = subprocess.run(
                [sys.executable, "-c", "import av_core.fstl_service as service; print(service.fstl.__file__)"],
                cwd=directory,
                env=environment,
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertEqual((install / "fstl_client_core.py").resolve(), Path(completed.stdout.strip()).resolve())


if __name__ == "__main__":
    unittest.main()

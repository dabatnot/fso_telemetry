from __future__ import annotations

import asyncio
import sys
import tempfile
import unittest
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from fastapi.testclient import TestClient  # noqa: E402

from av_core.app import create_app, status_event_stream  # noqa: E402
from av_core.config import ConfigStore  # noqa: E402
from av_core.models import AvCoreConfig  # noqa: E402
from av_core.runtime import AvCoreRuntime  # noqa: E402


class AvCoreApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.config_path = root / "av-core.json"
        self.frontend = root / "frontend"
        self.frontend.mkdir()
        (self.frontend / "index.html").write_text("<html>AV CORE</html>", encoding="utf-8")
        (self.frontend / "visible.txt").write_text("visible", encoding="utf-8")
        self.runtime = AvCoreRuntime(ConfigStore(self.config_path), actual_http_port=8080)
        self.app = create_app(self.runtime, self.frontend)
        self.client = TestClient(self.app)

    def test_config_round_trip_and_restart_required(self) -> None:
        initial = self.client.get("/api/config")
        self.assertEqual(200, initial.status_code)
        payload = initial.json()
        payload["web"]["port"] = 9090
        payload["modules"]["sensProc"]["installed"] = True

        updated = self.client.put("/api/config", json=payload)
        self.assertEqual(200, updated.status_code)
        self.assertTrue(updated.json()["restartRequired"])
        self.assertTrue(updated.json()["config"]["modules"]["sensProc"]["installed"])
        self.assertEqual(9090, self.client.get("/api/config").json()["web"]["port"])

    def test_invalid_config_returns_422_without_changing_state(self) -> None:
        payload = self.client.get("/api/config").json()
        payload["alerts"]["shield"]["clearAbovePercent"] = 10
        response = self.client.put("/api/config", json=payload)
        self.assertEqual(422, response.status_code)
        self.assertEqual(35, self.client.get("/api/config").json()["alerts"]["shield"]["clearAbovePercent"])

    def test_initial_status_is_explicitly_unavailable(self) -> None:
        status = self.client.get("/api/status").json()
        self.assertEqual("AvCoreStatusV1", status["schema"])
        self.assertEqual("OK", status["configuration"]["state"])
        self.assertEqual("UNAVAILABLE", status["telemetry"]["state"])
        self.assertEqual("UNAVAILABLE", status["can"]["state"])
        self.assertEqual("can0", status["can"]["interface"])
        self.assertEqual(1000000, status["can"]["bitrate"])
        self.assertTrue(all(module["state"] == "UNAVAILABLE" for module in status["modules"]))
        self.assertTrue(all(module["protocolId"] is None for module in status["modules"]))

    def test_corrupt_startup_config_is_reported_until_valid_save(self) -> None:
        self.config_path.write_text("broken", encoding="utf-8")
        runtime = AvCoreRuntime(ConfigStore(self.config_path))
        app = create_app(runtime, self.frontend)
        client = TestClient(app)
        self.assertEqual("ERROR", client.get("/api/status").json()["configuration"]["state"])
        response = client.put(
            "/api/config",
            json=AvCoreConfig().model_dump(mode="json", by_alias=True),
        )
        self.assertEqual(200, response.status_code)
        self.assertEqual("OK", client.get("/api/status").json()["configuration"]["state"])

    def test_lamp_test_is_unavailable_in_lot_one(self) -> None:
        response = self.client.post("/api/lamp-test", json={"target": "ALL", "active": True})
        self.assertEqual(503, response.status_code)
        self.assertEqual("CAN_UNAVAILABLE", response.json()["detail"])

    def test_static_files_are_served_without_directory_escape(self) -> None:
        self.assertIn("AV CORE", self.client.get("/").text)
        self.assertEqual("visible", self.client.get("/visible.txt").text)
        frontend_route = next(
            route for route in self.app.routes if getattr(route, "path", None) == "/{path:path}"
        )
        escaped = asyncio.run(frontend_route.endpoint("../outside.txt"))
        self.assertEqual((self.frontend / "index.html").resolve(), Path(escaped.path).resolve())

    def test_sse_stream_starts_with_complete_status(self) -> None:
        async def first_event() -> str:
            async def connected() -> bool:
                return False

            stream = status_event_stream(self.runtime, connected, poll_seconds=0)
            try:
                return await anext(stream)
            finally:
                await stream.aclose()

        event = asyncio.run(first_event())
        self.assertTrue(event.startswith("event: status\nid: 0\ndata: "))
        self.assertIn('"schema":"AvCoreStatusV1"', event)


if __name__ == "__main__":
    unittest.main()

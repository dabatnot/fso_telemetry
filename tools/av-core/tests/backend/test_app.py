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
from av_core.models import AvCoreConfig, CanStatus, ModuleStatus  # noqa: E402
from av_core.runtime import AvCoreRuntime  # noqa: E402


class FakeCanService:
    def __init__(self, config, on_change):
        self.config = config
        self.on_change = on_change
        self.online = False
        self.threat_online = False
        self.requests = []

    def start(self): pass
    def stop(self): pass
    def reconfigure(self, config): self.config = config
    def update_cockpit(self, cockpit, telemetry_state): pass
    def start_lamp_test(self, request):
        if not self.online:
            return "WARN_CTRL_UNAVAILABLE"
        if (request.target == "THREAT_PROC" or (request.lamp or "").startswith("THREAT_")) and not self.threat_online:
            return "THREAT_PROC_UNAVAILABLE"
        self.requests.append(request)
        return None
    def status(self): return CanStatus(state="OK" if self.online else "UNAVAILABLE")
    def modules(self):
        roles = ("WARN_CTRL", "THREAT_PROC", "SENS_PROC", "INST_PROC")
        return [ModuleStatus(role=role, installed=True, state="ONLINE" if (self.online and role == "WARN_CTRL") or (self.threat_online and role == "THREAT_PROC") else "UNAVAILABLE", protocol_id=0x700 + index) for index, role in enumerate(roles)]


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
        self.runtime = AvCoreRuntime(ConfigStore(self.config_path), actual_http_port=8080, can_factory=FakeCanService)
        self.can = self.runtime._can
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

    def test_initial_status_is_disconnected_and_hardware_is_unavailable(self) -> None:
        status = self.client.get("/api/status").json()
        self.assertEqual("AvCoreStatusV1", status["schema"])
        self.assertEqual("OK", status["configuration"]["state"])
        self.assertEqual("DISCONNECTED", status["telemetry"]["state"])
        self.assertFalse(status["cockpit"]["available"])
        self.assertEqual("UNAVAILABLE", status["cockpit"]["cautions"]["engine"]["state"])
        self.assertEqual("UNAVAILABLE", status["can"]["state"])
        self.assertEqual("can0", status["can"]["interface"])
        self.assertEqual(1000000, status["can"]["bitrate"])
        self.assertTrue(all(module["state"] == "UNAVAILABLE" for module in status["modules"]))
        self.assertEqual([0x700, 0x701, 0x702, 0x703], [module["protocolId"] for module in status["modules"]])

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

    def test_lamp_test_requires_can_and_warn_ctrl(self) -> None:
        response = self.client.post("/api/lamp-test", json={"target": "ALL"})
        self.assertEqual(503, response.status_code)
        self.assertEqual("WARN_CTRL_UNAVAILABLE", response.json()["detail"])

    def test_lamp_test_accepts_all_warn_ctrl_and_one_known_lamp(self) -> None:
        self.can.online = True
        for payload in ({"target": "ALL"}, {"target": "WARN_CTRL"}, {"target": "LAMP", "lamp": "MISSILE"}):
            self.assertEqual(200, self.client.post("/api/lamp-test", json=payload).status_code)
        self.assertEqual(["ALL", "WARN_CTRL", "LAMP"], [request.target for request in self.can.requests])
        self.assertEqual(422, self.client.post("/api/lamp-test", json={"target": "LAMP"}).status_code)
        self.assertEqual(422, self.client.post("/api/lamp-test", json={"target": "ALL", "lamp": "FIRE"}).status_code)

    def test_threat_lamp_tests_require_threat_proc(self) -> None:
        self.can.online = True
        unavailable = self.client.post("/api/lamp-test", json={"target": "THREAT_PROC"})
        self.assertEqual(503, unavailable.status_code)
        self.assertEqual("THREAT_PROC_UNAVAILABLE", unavailable.json()["detail"])
        self.can.threat_online = True
        for payload in (
            {"target": "THREAT_PROC"},
            {"target": "LAMP", "lamp": "THREAT_LOCK"},
        ):
            self.assertEqual(200, self.client.post("/api/lamp-test", json=payload).status_code)

    def test_static_files_are_served_without_directory_escape(self) -> None:
        index = self.client.get("/")
        self.assertIn("AV CORE", index.text)
        self.assertEqual("no-cache", index.headers["cache-control"])
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

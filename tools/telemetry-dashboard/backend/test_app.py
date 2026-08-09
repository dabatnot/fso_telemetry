from __future__ import annotations

import asyncio
import unittest
from pathlib import Path
from typing import Any

from fastapi import HTTPException

from app import CaptureStart, ReplayControl, ReplayUdpSettings, create_app


class FakeRuntime:
    def __init__(self) -> None:
        self.fail = False
        self.udp_running = False
        self.started_capture: tuple[str | None, int | None] | None = None

    def start(self) -> None:
        pass

    def stop(self) -> None:
        pass

    def request_live_resync(self) -> dict[str, Any]:
        if self.fail:
            raise ValueError("resync requires an active session")
        return {"accepted": True, "action": "resync", "pending": False}

    def request_live_reconnect(self) -> dict[str, Any]:
        if self.fail:
            raise ValueError("reconnect is available only in live mode")
        return {"accepted": True, "action": "reconnect", "pending": False}

    def start_capture(self, *, name: str | None = None, expected_duration_us: int | None = None) -> Path:
        self.started_capture = (name, expected_duration_us)
        return Path("test-capture.fstlcap")

    def replay_control(self, **values: Any) -> dict[str, Any]:
        if values.get("speed") not in (None, 1.0):
            raise ValueError("replay speed is fixed at 1.0")
        return values

    def replay_udp_settings(self, *, bind_host: str, port: int, lan_enabled: bool) -> dict[str, Any]:
        return {"bindHost": bind_host, "port": port, "lanEnabled": lan_enabled}

    def start_replay_udp(self) -> dict[str, Any]:
        self.udp_running = True
        return {"running": True}

    def stop_replay_udp(self) -> dict[str, Any]:
        self.udp_running = False
        return {"running": False}


class DashboardLiveApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.runtime = FakeRuntime()
        self.app = create_app(self.runtime)  # type: ignore[arg-type]

    def route(self, path: str):
        return next(route for route in self.app.routes if getattr(route, "path", None) == path)

    def test_live_commands_are_explicit_accepted_actions(self) -> None:
        resync = self.route("/api/live/resync")
        reconnect = self.route("/api/live/reconnect")
        self.assertEqual(202, resync.status_code)
        self.assertEqual(202, reconnect.status_code)
        self.assertEqual("resync", asyncio.run(resync.endpoint())["action"])
        self.assertEqual("reconnect", asyncio.run(reconnect.endpoint())["action"])

    def test_capture_library_start_is_a_real_post_endpoint(self) -> None:
        route = self.route("/api/captures/start")
        self.assertIn("POST", route.methods)
        result = asyncio.run(
            route.endpoint(CaptureStart(name="Test Capture", expectedDurationUs=600_000_000))
        )
        self.assertEqual({"path": "test-capture.fstlcap"}, result)
        self.assertEqual(("Test Capture", 600_000_000), self.runtime.started_capture)

    def test_live_command_conflicts_are_reported_as_409(self) -> None:
        self.runtime.fail = True
        for path in ("/api/live/resync", "/api/live/reconnect"):
            with self.subTest(path=path), self.assertRaises(HTTPException) as raised:
                asyncio.run(self.route(path).endpoint())
            self.assertEqual(409, raised.exception.status_code)

    def test_replay_control_is_time_based_and_rejects_fast_forward(self) -> None:
        control = self.route("/api/replay/control")
        result = asyncio.run(control.endpoint(ReplayControl(positionUs=1_500_000, speed=1)))
        self.assertEqual(1_500_000, result["position_us"])
        with self.assertRaises(HTTPException) as raised:
            asyncio.run(control.endpoint(ReplayControl(speed=2)))
        self.assertEqual(409, raised.exception.status_code)

    def test_replay_udp_settings_and_lifecycle_are_explicit(self) -> None:
        settings = self.route("/api/replay/udp/settings")
        result = asyncio.run(settings.endpoint(ReplayUdpSettings(bindHost="127.0.0.1", port=43000)))
        self.assertEqual(43000, result["port"])
        self.assertTrue(asyncio.run(self.route("/api/replay/udp/start").endpoint())["running"])
        self.assertFalse(asyncio.run(self.route("/api/replay/udp/stop").endpoint())["running"])


if __name__ == "__main__":
    unittest.main()

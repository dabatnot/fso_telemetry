from __future__ import annotations

import asyncio
import unittest
from typing import Any

from fastapi import HTTPException

from app import create_app


class FakeRuntime:
    def __init__(self) -> None:
        self.fail = False

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

    def test_live_command_conflicts_are_reported_as_409(self) -> None:
        self.runtime.fail = True
        for path in ("/api/live/resync", "/api/live/reconnect"):
            with self.subTest(path=path), self.assertRaises(HTTPException) as raised:
                asyncio.run(self.route(path).endpoint())
            self.assertEqual(409, raised.exception.status_code)


if __name__ == "__main__":
    unittest.main()

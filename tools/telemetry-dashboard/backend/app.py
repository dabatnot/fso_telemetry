"""FastAPI bridge for the FS2Open simpit validation dashboard."""

from __future__ import annotations

import argparse
import asyncio
import json
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from dashboard_runtime import REPO_ROOT, TelemetryRuntime


APP_ROOT = Path(__file__).resolve().parents[1]
FRONTEND_DIST = APP_ROOT / "frontend" / "dist"
CATALOG_PATH = APP_ROOT / "frontend" / "src" / "instrument-catalog.json"


class ReplayControl(BaseModel):
    playing: bool | None = None
    speed: float | None = None
    position: int | None = None


def load_catalog() -> list[dict[str, Any]]:
    return json.loads(CATALOG_PATH.read_text(encoding="utf-8"))


def create_app(runtime: TelemetryRuntime) -> FastAPI:
    @asynccontextmanager
    async def lifespan(_: FastAPI):
        runtime.start()
        try:
            yield
        finally:
            runtime.stop()

    app = FastAPI(title="FSO Telemetry Dashboard", version="1", lifespan=lifespan)

    @app.get("/api/status")
    async def status() -> dict[str, Any]:
        return runtime.latest()

    @app.get("/api/catalog")
    async def catalog() -> list[dict[str, Any]]:
        return load_catalog()

    @app.post("/api/capture/start")
    async def capture_start() -> dict[str, str]:
        try:
            return {"path": str(runtime.start_capture())}
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/capture/stop")
    async def capture_stop() -> dict[str, str | None]:
        return {"path": str(runtime.stop_capture()) if runtime.capture.active else None}

    @app.post("/api/live/resync", status_code=202)
    async def live_resync() -> dict[str, Any]:
        try:
            return runtime.request_live_resync()
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/live/reconnect", status_code=202)
    async def live_reconnect() -> dict[str, Any]:
        try:
            return runtime.request_live_reconnect()
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/replay/control")
    async def replay_control(control: ReplayControl) -> dict[str, Any]:
        try:
            return runtime.replay_control(
                playing=control.playing,
                speed=control.speed,
                position=control.position,
            )
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/export")
    async def export() -> dict[str, str]:
        return runtime.export(
            REPO_ROOT / "build" / "telemetry-dashboard" / "exports",
            load_catalog(),
        )

    @app.websocket("/api/ws")
    async def websocket_endpoint(websocket: WebSocket) -> None:
        await websocket.accept()
        last_version = -1
        try:
            while True:
                version = runtime.version
                if version != last_version:
                    await websocket.send_json(runtime.latest())
                    last_version = version
                await asyncio.sleep(1.0 / 30.0)
        except WebSocketDisconnect:
            return

    if FRONTEND_DIST.is_dir():
        assets = FRONTEND_DIST / "assets"
        if assets.is_dir():
            app.mount("/assets", StaticFiles(directory=assets), name="assets")

        @app.get("/{path:path}")
        async def frontend(path: str) -> FileResponse:
            candidate = (FRONTEND_DIST / path).resolve()
            if path and candidate.is_file() and FRONTEND_DIST.resolve() in candidate.parents:
                return FileResponse(candidate)
            return FileResponse(FRONTEND_DIST / "index.html")

    return app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="FSO simpit telemetry dashboard bridge")
    parser.add_argument("--telemetry-host", default="127.0.0.1")
    parser.add_argument("--telemetry-port", type=int, default=42042)
    parser.add_argument("--ui-port", type=int, default=43100)
    parser.add_argument("--flight-hz", type=int, default=30)
    parser.add_argument("--systems-hz", type=int, default=10)
    parser.add_argument("--mission-heartbeat-ms", type=int, default=500)
    parser.add_argument("--replay", type=Path)
    args = parser.parse_args()
    if not 1 <= args.telemetry_port <= 65535 or not 1 <= args.ui_port <= 65535:
        parser.error("ports must be in 1..65535")
    if not 1 <= args.flight_hz <= 60 or not 1 <= args.systems_hz <= 60:
        parser.error("cadences must be in 1..60 Hz")
    if args.mission_heartbeat_ms < 1:
        parser.error("mission heartbeat must be positive")
    if args.replay is not None and not args.replay.is_file():
        parser.error("replay capture does not exist")
    return args


def main() -> None:
    import uvicorn

    args = parse_args()
    runtime = TelemetryRuntime(
        host=args.telemetry_host,
        port=args.telemetry_port,
        flight_hz=args.flight_hz,
        systems_hz=args.systems_hz,
        mission_heartbeat_ms=args.mission_heartbeat_ms,
        capture_dir=REPO_ROOT / "build" / "telemetry-dashboard" / "captures",
        replay_path=args.replay,
    )
    uvicorn.run(create_app(runtime), host="127.0.0.1", port=args.ui_port, log_level="info")


if __name__ == "__main__":
    main()

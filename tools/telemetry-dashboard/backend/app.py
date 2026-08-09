"""FastAPI bridge for the FS2Open simpit validation dashboard."""

from __future__ import annotations

import argparse
import asyncio
import json
import shutil
import tempfile
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from dashboard_runtime import REPO_ROOT, TelemetryRuntime, default_capture_directory


APP_ROOT = Path(__file__).resolve().parents[1]
FRONTEND_DIST = APP_ROOT / "frontend" / "dist"
CATALOG_PATH = APP_ROOT / "frontend" / "src" / "instrument-catalog.json"


class ReplayControl(BaseModel):
    playing: bool | None = None
    speed: float | None = None
    position: int | None = None
    positionUs: int | None = None
    activeRangeId: str | None = None
    setActiveRange: bool = False
    loop: bool | None = None


class ReplayPreview(BaseModel):
    positionUs: int


class CaptureStart(BaseModel):
    name: str | None = None
    expectedDurationUs: int | None = None


class CaptureRename(BaseModel):
    name: str


class CaptureRangeBody(BaseModel):
    name: str
    startUs: int
    endUs: int


class ReplayUdpSettings(BaseModel):
    bindHost: str = "127.0.0.1"
    port: int = 42042
    lanEnabled: bool = False


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
    @app.post("/api/captures/start")
    async def capture_start(body: CaptureStart | None = None) -> dict[str, str]:
        try:
            body = body or CaptureStart()
            return {"path": str(runtime.start_capture(name=body.name, expected_duration_us=body.expectedDurationUs))}
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/capture/stop")
    @app.post("/api/captures/stop")
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
                position_us=control.positionUs,
                active_range_id=control.activeRangeId if control.setActiveRange else ...,
                loop=control.loop,
            )
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/replay/live")
    async def replay_live() -> dict[str, Any]:
        return runtime.return_to_live()

    @app.post("/api/replay/preview", status_code=202)
    async def replay_preview(body: ReplayPreview) -> dict[str, Any]:
        try:
            return runtime.replay_preview(body.positionUs)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/replay/udp/settings")
    async def replay_udp_settings(body: ReplayUdpSettings) -> dict[str, Any]:
        try:
            return runtime.replay_udp_settings(
                bind_host=body.bindHost, port=body.port, lan_enabled=body.lanEnabled
            )
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/replay/udp/start")
    async def replay_udp_start() -> dict[str, Any]:
        try:
            return runtime.start_replay_udp()
        except (OSError, ValueError) as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.post("/api/replay/udp/stop")
    async def replay_udp_stop() -> dict[str, Any]:
        return runtime.stop_replay_udp()

    @app.get("/api/captures")
    async def captures(q: str = "") -> list[dict[str, Any]]:
        return runtime.captures(q)

    @app.post("/api/captures/import")
    async def capture_import(request: Request, filename: str = "capture.fstlcap") -> dict[str, Any]:
        maximum_bytes = runtime.capture.stop_bytes
        reserve_bytes = runtime.capture.free_reserve_bytes
        content_length = request.headers.get("content-length")
        if content_length is not None:
            try:
                declared_bytes = int(content_length)
            except ValueError as exc:
                raise HTTPException(status_code=400, detail="invalid Content-Length") from exc
            if maximum_bytes > 0 and declared_bytes > maximum_bytes:
                raise HTTPException(status_code=413, detail="capture upload exceeds the configured size limit")
        suffix = ".fstlcap.jsonl" if filename.lower().endswith(".jsonl") else ".fstlcap"
        with tempfile.TemporaryDirectory(dir=runtime.capture_library.root) as directory:
            source = Path(directory) / f"upload{suffix}"
            received_bytes = 0
            with source.open("wb") as stream:
                async for chunk in request.stream():
                    if not chunk:
                        continue
                    received_bytes += len(chunk)
                    if maximum_bytes > 0 and received_bytes > maximum_bytes:
                        raise HTTPException(status_code=413, detail="capture upload exceeds the configured size limit")
                    if shutil.disk_usage(runtime.capture_library.root).free - len(chunk) < reserve_bytes:
                        raise HTTPException(status_code=507, detail="capture upload would exceed the free-space reserve")
                    await asyncio.to_thread(stream.write, chunk)
            if received_bytes == 0:
                raise HTTPException(status_code=422, detail="empty capture upload")
            try:
                return await asyncio.to_thread(runtime.capture_library.import_path, source)
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/api/captures/{capture_id}/load")
    async def capture_load(capture_id: str) -> dict[str, Any]:
        try:
            return runtime.load_replay(capture_id)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.patch("/api/captures/{capture_id}")
    async def capture_rename(capture_id: str, body: CaptureRename) -> dict[str, Any]:
        try:
            return runtime.capture_library.rename(capture_id, body.name)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.get("/api/captures/{capture_id}/download")
    async def capture_download(capture_id: str) -> FileResponse:
        if runtime.capture.active and runtime.capture.capture_id == capture_id:
            raise HTTPException(status_code=409, detail="stop the active capture before downloading it")
        try:
            path = runtime.capture_library.resolve(capture_id)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc
        return FileResponse(path, filename=path.name, media_type="application/octet-stream")

    @app.delete("/api/captures/{capture_id}", status_code=204)
    async def capture_delete(capture_id: str) -> None:
        try:
            if runtime.capture.active and runtime.capture.capture_id == capture_id:
                raise ValueError("stop the active capture before deleting it")
            if runtime.mode == "replay" and runtime.replay_state.get("captureId") == capture_id:
                raise ValueError("return to live before deleting the loaded capture")
            runtime.capture_library.delete(capture_id)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.get("/api/captures/{capture_id}/ranges")
    async def capture_ranges(capture_id: str, q: str = "") -> list[dict[str, Any]]:
        try:
            return runtime.capture_library.ranges(capture_id, q)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc

    @app.post("/api/captures/{capture_id}/ranges", status_code=201)
    async def capture_range_create(capture_id: str, body: CaptureRangeBody) -> dict[str, Any]:
        try:
            return runtime.create_range(capture_id, body.name, body.startUs, body.endUs)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.patch("/api/captures/{capture_id}/ranges/{range_id}")
    async def capture_range_update(capture_id: str, range_id: str, body: CaptureRangeBody) -> dict[str, Any]:
        try:
            return runtime.update_range(capture_id, range_id, body.name, body.startUs, body.endUs)
        except ValueError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.delete("/api/captures/{capture_id}/ranges/{range_id}", status_code=204)
    async def capture_range_delete(capture_id: str, range_id: str) -> None:
        try:
            runtime.delete_range(capture_id, range_id)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc

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
    parser.add_argument("--capture-dir", type=Path, default=default_capture_directory())
    parser.add_argument("--capture-warning-gib", type=float, default=5.0)
    parser.add_argument("--capture-stop-gib", type=float, default=10.0)
    parser.add_argument("--capture-free-reserve-gib", type=float, default=2.0)
    args = parser.parse_args()
    if not 1 <= args.telemetry_port <= 65535 or not 1 <= args.ui_port <= 65535:
        parser.error("ports must be in 1..65535")
    if not 1 <= args.flight_hz <= 60 or not 1 <= args.systems_hz <= 60:
        parser.error("cadences must be in 1..60 Hz")
    if args.mission_heartbeat_ms < 1:
        parser.error("mission heartbeat must be positive")
    if min(args.capture_warning_gib, args.capture_stop_gib, args.capture_free_reserve_gib) < 0:
        parser.error("capture limits cannot be negative")
    if args.capture_stop_gib and args.capture_warning_gib > args.capture_stop_gib:
        parser.error("capture warning must not exceed the automatic stop limit")
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
        capture_dir=args.capture_dir.expanduser().resolve(),
        replay_path=args.replay,
        capture_warn_bytes=int(args.capture_warning_gib * 1024**3),
        capture_stop_bytes=int(args.capture_stop_gib * 1024**3),
        capture_free_reserve_bytes=int(args.capture_free_reserve_gib * 1024**3),
    )
    uvicorn.run(create_app(runtime), host="127.0.0.1", port=args.ui_port, log_level="info")


if __name__ == "__main__":
    main()

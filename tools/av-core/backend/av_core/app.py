from __future__ import annotations

import argparse
import asyncio
import json
import time
from collections.abc import AsyncIterator, Awaitable, Callable
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from .config import ConfigStore
from .models import AvCoreConfig, AvCoreStatus, ConfigUpdateResponse, LampTestRequest
from .runtime import AvCoreRuntime


DEFAULT_CONFIG_PATH = Path("/var/lib/fsotelemetry/av-core.json")


def public_json(model: object) -> str:
    if hasattr(model, "model_dump"):
        payload = model.model_dump(mode="json", by_alias=True)  # type: ignore[union-attr]
    else:
        payload = model
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


async def status_event_stream(
    runtime: AvCoreRuntime,
    disconnected: Callable[[], Awaitable[bool]],
    *,
    poll_seconds: float = 0.25,
    keepalive_seconds: float = 15.0,
) -> AsyncIterator[str]:
    last_revision = -1
    last_emit = time.monotonic()
    while not await disconnected():
        revision = runtime.revision
        if revision != last_revision:
            yield f"event: status\nid: {revision}\ndata: {public_json(runtime.status())}\n\n"
            last_revision = revision
            last_emit = time.monotonic()
        elif time.monotonic() - last_emit >= keepalive_seconds:
            yield ": keepalive\n\n"
            last_emit = time.monotonic()
        await asyncio.sleep(poll_seconds)


def create_app(runtime: AvCoreRuntime, frontend_dir: Path) -> FastAPI:
    @asynccontextmanager
    async def lifespan(_: FastAPI):
        runtime.start()
        try:
            yield
        finally:
            runtime.stop()

    app = FastAPI(title="FSO SimPit AV CORE", version="1", lifespan=lifespan)

    @app.get("/api/config", response_model=AvCoreConfig)
    async def get_config() -> AvCoreConfig:
        return runtime.config

    @app.put("/api/config", response_model=ConfigUpdateResponse)
    async def put_config(config: AvCoreConfig) -> ConfigUpdateResponse:
        try:
            return runtime.update_config(config)
        except OSError as exc:
            raise HTTPException(status_code=500, detail="CONFIG_WRITE_FAILED") from exc

    @app.get("/api/status", response_model=AvCoreStatus)
    async def get_status() -> AvCoreStatus:
        return runtime.status()

    @app.get("/api/events")
    async def get_events(request: Request) -> StreamingResponse:
        return StreamingResponse(
            status_event_stream(runtime, request.is_disconnected),
            media_type="text/event-stream",
            headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        )

    @app.post("/api/lamp-test")
    async def lamp_test(request: LampTestRequest) -> None:
        unavailable = runtime.start_lamp_test(request)
        if unavailable is not None:
            raise HTTPException(status_code=503, detail=unavailable)

    root = frontend_dir.resolve()
    assets = root / "assets"
    if assets.is_dir():
        app.mount("/assets", StaticFiles(directory=assets), name="assets")

    @app.get("/{path:path}")
    async def frontend(path: str) -> FileResponse:
        index = root / "index.html"
        candidate = (root / path).resolve()
        if path and candidate.is_relative_to(root) and candidate.is_file():
            if candidate == index:
                return FileResponse(candidate, headers={"Cache-Control": "no-cache"})
            return FileResponse(candidate)
        if not index.is_file():
            raise HTTPException(status_code=503, detail="FRONTEND_UNAVAILABLE")
        return FileResponse(index, headers={"Cache-Control": "no-cache"})

    return app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="FSO SimPit AV CORE")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH)
    parser.add_argument("--frontend-dir", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    return parser.parse_args()


def main() -> None:
    import uvicorn

    args = parse_args()
    store = ConfigStore(args.config.expanduser().resolve())
    runtime = AvCoreRuntime(store)
    uvicorn.run(
        create_app(runtime, args.frontend_dir.expanduser().resolve()),
        host=args.host,
        port=runtime.config.web.port,
        log_level="info",
    )


if __name__ == "__main__":
    main()

"""In-memory telemetry receiver and public, read-only Elwynn viewer."""
from __future__ import annotations

import json
import os
import secrets
import time
from pathlib import Path

from fastapi import FastAPI, HTTPException, Query, Request, Response
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import ValidationError

from .telemetry import MAX_REQUEST_BYTES, STALE_AFTER_MS, TelemetryBatch
from .memory import MemoryPages

APP_DIR = Path(__file__).parent


def create_app(telemetry_token: str | None = None) -> FastAPI:
    app = FastAPI(title="AI World Viewer", version="3.0.0")
    app.mount("/static", StaticFiles(directory=APP_DIR / "static"), name="static")
    app.state.telemetry_token = (
        telemetry_token if telemetry_token is not None else os.getenv("WORLD_VIEWER_TELEMETRY_TOKEN", "")
    )
    app.state.snapshot = None
    app.state.received_at_ms = None
    app.state.received_monotonic = None
    app.state.agent_ids = set()
    app.state.memory_pages = MemoryPages()

    @app.get("/health")
    def health() -> dict:
        return {"status": "ok", "telemetry_configured": bool(app.state.telemetry_token)}

    @app.get("/", include_in_schema=False)
    def index() -> FileResponse:
        return FileResponse(APP_DIR / "static" / "index.html")

    @app.post("/internal/telemetry")
    async def receive(request: Request, response: Response) -> dict:
        token = app.state.telemetry_token
        if not token:
            raise HTTPException(status_code=503, detail="Telemetry token is not configured")
        authorization = request.headers.get("authorization", "")
        scheme, separator, supplied = authorization.partition(" ")
        if not separator or scheme.lower() != "bearer" or not secrets.compare_digest(supplied, token):
            raise HTTPException(status_code=401, detail="Invalid telemetry bearer token")

        # Read as a stream: a false or absent Content-Length cannot bypass the cap.
        body = bytearray()
        async for chunk in request.stream():
            body.extend(chunk)
            if len(body) > MAX_REQUEST_BYTES:
                raise HTTPException(status_code=413, detail="Telemetry payload is too large")
        try:
            data = json.loads(body)
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise HTTPException(status_code=400, detail="Invalid JSON") from None
        try:
            snapshot = TelemetryBatch.model_validate(data)
        except ValidationError as exc:
            raise RequestValidationError(exc.errors()) from exc
        agent_ids = [agent.agent_id for agent in snapshot.agents]
        if len(agent_ids) != len(set(agent_ids)):
            raise HTTPException(status_code=422, detail="Duplicate agent_id in snapshot")

        # A full batch replaces the prior one, so vanished agents disappear.
        app.state.snapshot = snapshot
        app.state.received_at_ms = int(time.time() * 1000)
        app.state.received_monotonic = time.monotonic()
        app.state.agent_ids = set(agent_ids)
        if snapshot.version < 3:
            app.state.memory_pages = MemoryPages()
        else:
            pages = app.state.memory_pages
            pages.prune(app.state.received_monotonic, app.state.agent_ids)
            pages.accept(snapshot.memory_page, app.state.received_monotonic, snapshot.captured_at_ms)
            if header := pages.next_header():
                response.headers["X-Observer-Memory-Request"] = header
        return {"accepted": len(agent_ids)}

    @app.get("/api/agents/{agent_id}/memory")
    async def memory(agent_id: int, response: Response,
                     offset: int = Query(default=0, ge=0, le=2**32 - 1),
                     anchor: int = Query(default=0, ge=0, le=2**32 - 1),
                     refresh: bool = False) -> dict:
        response.headers["Cache-Control"] = "no-store"
        if agent_id not in app.state.agent_ids:
            raise HTTPException(status_code=404, detail="Agent is not in the current Observer snapshot")
        if app.state.snapshot.version < 3:
            return {"status": "unsupported"}
        now = time.monotonic()
        if now - app.state.received_monotonic >= STALE_AFTER_MS / 1000:
            return {"status": "offline"}
        pages = app.state.memory_pages
        pages.prune(now, app.state.agent_ids)
        return pages.read(agent_id, offset, anchor, refresh, now)

    @app.get("/api/state")
    def state() -> dict:
        snapshot: TelemetryBatch | None = app.state.snapshot
        received_monotonic: float | None = app.state.received_monotonic
        age_ms = max(0, int((time.monotonic() - received_monotonic) * 1000)) if received_monotonic is not None else None
        return {
            "version": snapshot.version if snapshot is not None else 3,
            "configured": bool(app.state.telemetry_token),
            "captured_at_ms": snapshot.captured_at_ms if snapshot is not None else None,
            "received_at_ms": app.state.received_at_ms,
            "age_ms": age_ms,
            "stale": age_ms is None or age_ms >= STALE_AFTER_MS,
            "agents": [agent.model_dump(mode="json") for agent in snapshot.agents] if snapshot is not None else [],
        }

    return app


app = create_app()

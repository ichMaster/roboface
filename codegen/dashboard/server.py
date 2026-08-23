"""Dashboard server — tails the log, serves the page, pushes over a WebSocket.

Port **8420**, never 8000: the generated application owns that one and both may run at
once. Reads only ``codegen/runs/``, writes only ``codegen/var/`` (vision §3 principle 6).

It imports nothing from ``server/``, ``games/`` or ``agent/`` — those directories are
regenerated and deleted, and this must start and serve with the whole application tree
absent, which is its normal state between runs.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from tracker import history, paths
from tracker.reduce import reduce
from tracker.state import read as read_state

PORT = 8420
STATIC = Path(__file__).resolve().parent / "static"

#: How often to look for new lines. The log is appended to in bursts, and the UI
#: debounces anyway (dashboard-specification §6.3), so polling beats a watcher here —
#: and costs no third-party dependency.
POLL_SECONDS = 0.4

#: Send a frame at least this often, even when the log has not grown.
#:
#: Frames used to be sent ONLY on growth, and a real run is quiet for long stretches: in
#: the v01–v03 run, 23 gaps ran over a minute and the longest was 26. With no frame, the
#: header's elapsed clock stops — it is computed at reduce time — so the page sat
#: motionless for 26 minutes while the pipeline was working and the socket was fine.
#: A stopped clock beside a "live" indicator is not a slow dashboard, it is a wrong one.
#:
#: The cost is one reduction per interval per client: ~10 ms for a 1000-event log. A
#: finished run needs no special case — its elapsed is measured to `ended`, not to now,
#: so the clock stops on its own and the repeated frames are identical.
HEARTBEAT_SECONDS = 5.0

app = FastAPI(title="Codegen Tracker", version="1")


def active_run_id() -> str | None:
    pointer = paths.current_pointer()
    if not pointer.is_file():
        return None
    return pointer.read_text(encoding="utf-8").strip() or None


def latest_run_id() -> str | None:
    """The active run, or the most recent one when nothing is running."""
    current = active_run_id()
    if current:
        return current
    runs = sorted(p.name for p in paths.runs_root().glob("run-*") if p.is_dir())
    return runs[-1] if runs else None


def current_state(run_id: str | None = None) -> dict[str, Any]:
    """Reduced state for a run, rebuilt from the log if no snapshot exists."""
    run_id = run_id or latest_run_id()
    if not run_id:
        return {"run_id": None, "status": "no-runs", "tree": [], "metrics": {}}
    snapshot = read_state(run_id)
    if snapshot is not None:
        return snapshot
    events = paths.events_path(run_id)
    lines = events.read_text(encoding="utf-8").splitlines() if events.is_file() else []
    state = reduce(lines, datetime.now(UTC))
    state.run_id = run_id
    return state.as_dict()


@app.get("/api/state")
def api_state(run_id: str | None = None) -> JSONResponse:
    """The same object the WebSocket sends, for a no-JS read."""
    return JSONResponse(current_state(run_id))


@app.get("/api/runs")
def api_runs() -> JSONResponse:
    runs = sorted((p.name for p in paths.runs_root().glob("run-*") if p.is_dir()), reverse=True)
    return JSONResponse({"runs": runs, "active": active_run_id()})


@app.get("/api/history")
def api_history() -> JSONResponse:
    """Cross-run comparison. `single_run` tells the UI to say so rather than draw a point."""
    return JSONResponse(history.comparison())


@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC / "index.html")


@app.websocket("/ws")
async def websocket(ws: WebSocket) -> None:
    """Snapshot on connect, then a delta per change.

    The client re-requests a snapshot on reconnect rather than resuming, so the server
    keeps no per-client cursor (dashboard-specification §6.2).
    """
    await ws.accept()
    try:
        run_id = latest_run_id()
        await ws.send_json({"kind": "snapshot", "state": current_state(run_id)})

        # Seed from the size the snapshot was built at, not -1. At -1 the first poll
        # always fired, sending a duplicate of the snapshot a tick after it -- harmless
        # in itself, but it also meant a test could receive that frame and believe the
        # heartbeat worked when it did not.
        events_now = paths.events_path(run_id) if run_id else None
        last_size = events_now.stat().st_size if events_now and events_now.is_file() else 0
        last_sent = time.monotonic()
        while True:
            await asyncio.sleep(POLL_SECONDS)
            run_id = latest_run_id()
            if not run_id:
                continue
            events = paths.events_path(run_id)
            size = events.stat().st_size if events.is_file() else 0
            due = time.monotonic() - last_sent >= HEARTBEAT_SECONDS
            if size == last_size and not due:
                continue
            last_size = size
            last_sent = time.monotonic()
            await ws.send_json({"kind": "delta", "state": current_state(run_id)})
    except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
        pass
    finally:
        # Cleanup in a finally: a client-initiated drop can surface as cancellation
        # rather than WebSocketDisconnect.
        with contextlib.suppress(RuntimeError):
            await ws.close()


if STATIC.is_dir():
    app.mount("/static", StaticFiles(directory=str(STATIC)), name="static")


def main() -> int:  # pragma: no cover - process entry point
    import uvicorn

    paths.var_root().mkdir(parents=True, exist_ok=True)
    (paths.var_root() / "dashboard.pid").write_text(str(os.getpid()), encoding="utf-8")
    uvicorn.run(app, host="127.0.0.1", port=PORT, log_level="warning")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())


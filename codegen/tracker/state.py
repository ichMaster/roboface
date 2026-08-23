"""Persist and rebuild ``state.json``.

Written atomically — a temp file in the same directory, then ``os.replace``. The
dashboard polls this file while the pipeline appends to the log, so a reader must
never observe a half-written snapshot.

``state.json`` is disposable by design: deleting it loses nothing, because
:func:`rebuild` reconstructs it from ``events.jsonl``, which is the source of truth.
"""

from __future__ import annotations

import json
import os
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from tracker import paths
from tracker.reduce import State, reduce


def write(run_id: str, state: State) -> Path:
    """Atomically replace this run's ``state.json``."""
    target = paths.state_path(run_id)
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(state.as_dict(), indent=2, ensure_ascii=False, sort_keys=False) + "\n"

    handle, temp_name = tempfile.mkstemp(dir=str(target.parent), prefix=".state-", suffix=".tmp")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as fh:
            fh.write(payload)
        os.replace(temp_name, target)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise
    return target


def is_stale(run_id: str) -> bool:
    """Whether the log has grown since the snapshot was written.

    The snapshot is a cache and nothing invalidates it: no part of the pipeline writes
    ``state.json``, so in a normal run the file never exists and the dashboard reduces
    the log on every request. The moment anyone rebuilds one by hand, though, a reader
    that trusted it blindly would serve that frozen instant forever — still pushing
    WebSocket frames on every append, so the page *looks* live while its numbers never
    move. A stuck dashboard that advertises itself as live is worse than no dashboard.

    The log is append-only, so mtime is the whole signal. **A tie counts as stale:**
    reducing the log is always the correct answer and the snapshot is only ever an
    optimisation, so the cheap mistake is re-reducing once too often, not freezing.
    """
    snapshot = paths.state_path(run_id)
    if not snapshot.is_file():
        return True
    events = paths.events_path(run_id)
    if not events.is_file():
        return False
    return events.stat().st_mtime_ns >= snapshot.stat().st_mtime_ns


def read(run_id: str) -> dict[str, Any] | None:
    """The snapshot, or ``None`` when there isn't a usable one.

    ``None`` also means *stale* — callers already fall back to reducing the log, which
    is the source of truth, so a stale snapshot and a missing one deserve one answer.
    """
    path = paths.state_path(run_id)
    if not path.is_file() or is_stale(run_id):
        return None
    try:
        with path.open(encoding="utf-8") as fh:
            result: dict[str, Any] = json.load(fh)
        return result
    except (OSError, json.JSONDecodeError):
        return None


def rebuild(run_id: str, now: datetime | None = None) -> State:
    """Reduce this run's log and persist the result."""
    events = paths.events_path(run_id)
    lines = events.read_text(encoding="utf-8").splitlines() if events.is_file() else []
    state = reduce(lines, now or datetime.now(UTC))
    state.run_id = run_id
    write(run_id, state)
    return state


def _main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        prog="tracker.state", description="Rebuild state.json from a run's event log."
    )
    parser.add_argument("run_id", nargs="?", help="defaults to the active run")
    args = parser.parse_args(argv)

    run_id = args.run_id
    if not run_id:
        pointer = paths.current_pointer()
        run_id = pointer.read_text(encoding="utf-8").strip() if pointer.is_file() else ""
    if not run_id:
        print("no run id given and no active run", flush=True)
        return 1

    state = rebuild(run_id)
    print(
        f"{run_id}: {state.counts.get('events', 0)} events, "
        f"{len(state.quarantine)} quarantined, status {state.status}",
        flush=True,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess
    import sys

    sys.exit(_main(sys.argv[1:]))

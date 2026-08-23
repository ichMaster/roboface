"""Run lifecycle: allocate, resume, supersede, finish.

An interrupted run is the *normal* outcome of a failed gate or a killed session, not
an error state (architecture §9.3). So this module never refuses and never decides:
it exposes :func:`pending` so the orchestrator can show the user what it found, and
:func:`resume` / :func:`supersede` so either answer can be carried out.

Elapsed excludes idle. ``run.resumed`` carries ``gap_s``, which the reducer subtracts
from every elapsed figure — a run paused overnight must not report fourteen hours of
velocity.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from tracker import emit, paths

EMITTER = "skill:ship-phase"

#: Events that close a run. Anything else leaves it open.
TERMINAL = {"run.end", "run.aborted"}


@dataclass(frozen=True)
class Pending:
    """Summary of an interrupted run, for showing the user before they choose."""

    run_id: str
    command: str
    started: str
    last_event_ts: str
    last_released: str | None
    open_nodes: list[str] = field(default_factory=list)
    event_count: int = 0

    def gap_seconds(self, now: datetime | None = None) -> int:
        """Wall-clock idle since the last event."""
        moment = now or datetime.now(UTC)
        try:
            last = datetime.strptime(self.last_event_ts, "%Y-%m-%dT%H:%M:%S.%f%z")
        except ValueError:
            return 0
        return max(0, int((moment - last).total_seconds()))


def new_run_id(now: datetime | None = None) -> str:
    moment = now or datetime.now(UTC)
    return moment.strftime("run-%Y%m%d-%H%M%S")


def git_context() -> dict[str, str | None]:
    """Branch, HEAD and remote. Degrades to nulls rather than failing a run."""

    def _run(*args: str) -> str | None:
        try:
            out = subprocess.run(
                args, capture_output=True, text=True, timeout=5, cwd=paths.codegen_root().parent
            )
            return out.stdout.strip() or None if out.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    return {
        "branch": _run("git", "rev-parse", "--abbrev-ref", "HEAD"),
        "head_sha": _run("git", "rev-parse", "--short", "HEAD"),
        "remote": _run("git", "remote", "get-url", "origin"),
    }


def _read_events(run_id: str) -> list[dict[str, Any]]:
    path = paths.events_path(run_id)
    if not path.exists():
        return []
    events: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue  # a torn tail is the reducer's problem, not this one's
    return events


def pending() -> Pending | None:
    """The interrupted run, if there is one. ``None`` when nothing is open."""
    pointer = paths.current_pointer()
    if not pointer.is_file():
        return None
    run_id = pointer.read_text(encoding="utf-8").strip()
    if not run_id:
        return None

    events = _read_events(run_id)
    if not events or any(e.get("type") in TERMINAL for e in events):
        return None

    start = next((e for e in events if e.get("type") == "run.start"), None)
    open_nodes = _open_nodes(events)
    released = [e for e in events if e.get("type") == "version.end"]
    return Pending(
        run_id=run_id,
        command=(start or {}).get("data", {}).get("command", "unknown"),
        started=(start or {}).get("ts", events[0].get("ts", "")),
        last_event_ts=events[-1].get("ts", ""),
        last_released=(released[-1]["scope"].get("version") if released else None),
        open_nodes=open_nodes,
        event_count=len(events),
    )


def _open_nodes(events: list[dict[str, Any]]) -> list[str]:
    """Nodes with a ``*.start`` and no matching terminator, outermost first."""
    ends = {"end", "skipped", "aborted", "decomposed"}
    started: dict[str, str] = {}
    for event in events:
        etype = str(event.get("type", ""))
        family, _, tail = etype.rpartition(".")
        if not family:
            continue
        key = f"{family}:{json.dumps(event.get('scope', {}), sort_keys=True)}"
        if tail == "start":
            started[key] = f"{family} {event.get('scope', {})}"
        elif tail in ends:
            started.pop(key, None)
    return list(started.values())


def start(
    command: str,
    plan: list[str],
    baseline: dict[str, int],
    *,
    resumes: str | None = None,
    now: datetime | None = None,
) -> str:
    """Allocate a run, point ``current`` at it, and emit ``run.start``."""
    run_id = new_run_id(now)
    paths.run_dir(run_id).mkdir(parents=True, exist_ok=True)
    paths.current_pointer().parent.mkdir(parents=True, exist_ok=True)
    paths.current_pointer().write_text(run_id, encoding="utf-8")

    data: dict[str, Any] = {
        "command": command,
        "plan": plan,
        "baseline": baseline,
        "git": git_context(),
    }
    if resumes:
        data["resumes"] = resumes
    emit.emit("run.start", emitter=EMITTER, data=data)
    return run_id


def resume(run_id: str, *, now: datetime | None = None) -> int:
    """Continue an interrupted run. Returns the idle gap in seconds."""
    info = pending()
    gap = info.gap_seconds(now) if info and info.run_id == run_id else 0
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    emit.emit("run.resumed", emitter=EMITTER, data={"gap_s": gap})
    return gap


def supersede(run_id: str, reason: str = "superseded") -> None:
    """Close an interrupted run so a fresh one can start, linked by ``resumes``."""
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    emit.emit("run.aborted", emitter=EMITTER, status="fail", data={"reason": reason})


def finish(versions_done: int, issues_done: int, status: str = "ok") -> None:
    """Close the active run."""
    emit.emit(
        "run.end",
        emitter=EMITTER,
        status=status,
        data={"versions_done": versions_done, "issues_done": issues_done},
    )


def current_events_path() -> Path | None:
    run_id = emit.current_run_id()
    return paths.events_path(run_id) if run_id else None

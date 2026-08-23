"""TRK-005 — run lifecycle, and the resume-or-supersede decision.

The design point being pinned: an interrupted run is normal, so this module must
*surface* the choice rather than refuse or decide. Both wrong answers are costly
(architecture §9.3), which is why nothing here picks one automatically.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest

from tracker import emit, paths, run


def _events(run_id: str) -> list[dict[str, Any]]:
    path = paths.events_path(run_id)
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x.strip()]


@pytest.fixture
def started(isolated_runs_dir: Path) -> str:
    return run.start("/ship-phase v01", ["v01.01", "v01.02"], {"tests": 0, "mypy_errors": 0})


def test_start_creates_the_run_the_pointer_and_a_valid_event(started: str) -> None:
    assert paths.run_dir(started).is_dir()
    assert paths.current_pointer().read_text(encoding="utf-8").strip() == started
    events = _events(started)
    assert [e["type"] for e in events] == ["run.start"]
    assert events[0]["data"]["plan"] == ["v01.01", "v01.02"]


def test_run_id_format_matches_the_schema(isolated_runs_dir: Path) -> None:
    moment = datetime(2026, 8, 3, 14, 20, 12, tzinfo=UTC)
    assert run.new_run_id(moment) == "run-20260803-142012"


def test_git_context_is_populated_in_this_repo(started: str) -> None:
    git = _events(started)[0]["data"]["git"]
    assert git["branch"] and git["head_sha"]


def test_git_context_degrades_to_nulls_rather_than_failing(
    isolated_runs_dir: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A non-git checkout must not stop a run from starting."""
    import subprocess

    def boom(*_a: object, **_k: object) -> None:
        raise OSError("no git here")

    monkeypatch.setattr(subprocess, "run", boom)
    context = run.git_context()
    assert context == {"branch": None, "head_sha": None, "remote": None}


# ── pending detection ────────────────────────────────────────────────────────


def test_pending_is_none_when_nothing_has_run(isolated_runs_dir: Path) -> None:
    assert run.pending() is None


def test_pending_is_none_after_a_clean_finish(started: str) -> None:
    run.finish(versions_done=2, issues_done=9)
    assert run.pending() is None


def test_pending_reports_what_the_interrupted_run_was(started: str) -> None:
    emit.emit("phase.start", emitter=run.EMITTER, scope={"phase": "v01"})
    emit.emit(
        "version.end", emitter=run.EMITTER, status="ok",
        scope={"phase": "v01", "version": "v01.01"}, data={"tag": "v01.01.00"},
    )
    emit.emit("version.start", emitter=run.EMITTER, scope={"phase": "v01", "version": "v01.02"})

    info = run.pending()
    assert info is not None
    assert info.run_id == started
    assert info.command == "/ship-phase v01"
    assert info.last_released == "v01.01"
    assert info.event_count == 4
    assert any("v01.02" in node for node in info.open_nodes), info.open_nodes


def test_pending_survives_a_torn_final_line(started: str) -> None:
    """A killed process leaves half a line; detection must still work."""
    with paths.events_path(started).open("a", encoding="utf-8") as fh:
        fh.write('{"v":1,"ts":"2026-08-03T14:2')
    assert run.pending() is not None


def test_gap_seconds_measures_idle_since_the_last_event(started: str) -> None:
    info = run.pending()
    assert info is not None
    later = datetime.strptime(info.last_event_ts, "%Y-%m-%dT%H:%M:%S.%f%z") + timedelta(hours=1)
    assert 3595 <= info.gap_seconds(later) <= 3605


# ── the two outcomes ─────────────────────────────────────────────────────────


def test_resume_keeps_the_run_and_records_the_gap(started: str) -> None:
    info = run.pending()
    assert info is not None
    later = datetime.strptime(info.last_event_ts, "%Y-%m-%dT%H:%M:%S.%f%z") + timedelta(minutes=30)

    gap = run.resume(started, now=later)

    assert 1795 <= gap <= 1805
    assert paths.current_pointer().read_text(encoding="utf-8").strip() == started
    assert [e["type"] for e in _events(started)] == ["run.start", "run.resumed"]
    assert _events(started)[-1]["data"]["gap_s"] == gap
    assert len(list(paths.runs_root().glob("run-*"))) == 1, "resume must not create a new run"


def test_supersede_closes_the_old_run_and_links_the_new_one(started: str) -> None:
    run.supersede(started)
    assert [e["type"] for e in _events(started)][-1] == "run.aborted"
    assert run.pending() is None, "a superseded run is no longer pending"

    fresh = run.start(
        "/ship-phase v01", ["v01.02"], {"tests": 41, "mypy_errors": 0},
        resumes=started, now=datetime(2026, 8, 3, 15, 0, 0, tzinfo=UTC),
    )
    assert fresh != started
    assert _events(fresh)[0]["data"]["resumes"] == started


def test_neither_outcome_is_chosen_automatically(started: str) -> None:
    """The module surfaces the choice; it must not act on its own.

    Calling pending() has no side effects: no event, no pointer change. If this ever
    stops being true, the orchestrator's question becomes a formality.
    """
    before = _events(started)
    pointer_before = paths.current_pointer().read_text(encoding="utf-8")

    for _ in range(3):
        run.pending()

    assert _events(started) == before
    assert paths.current_pointer().read_text(encoding="utf-8") == pointer_before

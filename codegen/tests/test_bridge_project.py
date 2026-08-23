"""M5-003 / M5-004 — the projection and the statistics behind it.

Two kinds of test here. The purity and guard tests pin *properties*: no clock, same
input same bytes, every frame fits one write. The rest pin *numbers* against
``run-20260815-213849``, because a statistic with no known-correct answer is a
statistic nobody can refactor.

Three of those numbers exist to prove the projection reads around defects in the
tracker's own instrumentation rather than waiting on them (vision §9).
"""

from __future__ import annotations

import ast
import json
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, cast

import pytest

from bridge import devices, frames, project, stats
from tests import gen_log
from tracker import reduce as reduce_mod

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)
REAL_RUN = Path(__file__).resolve().parent.parent / "runs" / "run-20260815-213849"

real_run_only = pytest.mark.skipif(
    not (REAL_RUN / "events.jsonl").is_file(),
    reason="runs/ is gitignored; the recorded run is not present in this checkout",
)


def _state(preset: str = "clean-run") -> dict[str, Any]:
    return reduce_mod.reduce(gen_log.preset(preset).splitlines(), NOW).as_dict()


def _real() -> dict[str, Any]:
    lines = (REAL_RUN / "events.jsonl").read_text().splitlines()
    return reduce_mod.reduce(lines, NOW).as_dict()


ALL_WANTS = [devices.WANT_NOTIFY, *devices.CORE2.screens]
IDS = [devices.WANT_NAMES[w] for w in ALL_WANTS]


# ── M5-003: the properties ───────────────────────────────────────────────────


@pytest.mark.parametrize("module", [project, stats])
def test_the_projection_reads_no_clock_no_env_and_no_files(module: Any) -> None:
    """Asserted over the AST, same as ``tracker.reduce``. A single ``datetime.now()``
    would make every golden frame unreproducible."""
    tree = ast.parse(Path(module.__file__).read_text())
    banned = {"now", "today", "time", "environ", "getenv", "open", "read_text"}
    for node in ast.walk(tree):
        if isinstance(node, ast.Attribute) and node.attr in banned:
            pytest.fail(f"{module.__name__} touches {node.attr}")
        if isinstance(node, ast.Name) and node.id in {"open", "input"}:
            pytest.fail(f"{module.__name__} calls {node.id}")


@pytest.mark.parametrize("want", ALL_WANTS, ids=IDS)
def test_projecting_twice_yields_identical_bytes(want: int) -> None:
    state = _state()
    first = frames.encode(project.project(state, devices.CORE2, want))
    second = frames.encode(project.project(state, devices.CORE2, want))
    assert first == second


@pytest.mark.parametrize("want", ALL_WANTS, ids=IDS)
def test_every_screen_produces_a_valid_frame(want: int) -> None:
    assert frames.validate(project.project(_state(), devices.CORE2, want)) == []


@pytest.mark.parametrize("preset", ["clean-run", "retry-run", "aborted-run", "no-review"])
@pytest.mark.parametrize("want", ALL_WANTS, ids=IDS)
def test_every_screen_survives_every_fixture(preset: str, want: int) -> None:
    """Including the damaged ones. A panel that crashes on an aborted run is a panel
    that fails exactly when you most want to look at it."""
    frame = project.project(_state(preset), devices.CORE2, want)
    assert frames.fits(frame) and frames.is_ascii(frame)


def test_a_board_refuses_a_screen_it_does_not_render() -> None:
    with pytest.raises(ValueError, match="does not render"):
        project.project(_state(), devices.STICKC, devices.SCREEN_BURNDOWN)


def test_the_narrower_board_truncates_where_the_wider_one_does_not() -> None:
    """The two boards differ *when the content requires it*, not unconditionally.

    Corrected from the plan's original criterion, which said the same state always
    yields different frames. With a short label both fit and the frames are byte
    identical — which is correct behaviour, not a bug. What actually differs is the
    budget, and that only shows when the text exceeds the narrower one.
    """
    state = _state()
    # The *last* completed version, since that is the one running_label reports.
    versions = [cast(dict[str, Any], n) for n in stats.walk(state["tree"])
                if n["kind"] == "version"]
    versions[-1]["id"] = "v01.02-with-a-very-long-version-name"
    wide = project.project(state, devices.CORE2, devices.SCREEN_NOW)
    narrow = project.project(state, devices.STICKC, devices.SCREEN_NOW)
    assert len(narrow["cur"]) <= devices.STICKC.chars_per_line
    assert len(wide["cur"]) <= devices.CORE2.chars_per_line
    assert narrow["cur"] != wide["cur"]


def test_the_brightness_ladder_reaches_the_frame() -> None:
    state = _state()
    fresh = project.project(state, devices.CORE2, devices.SCREEN_NOW, idle_s=0)
    dimmed = project.project(state, devices.CORE2, devices.SCREEN_NOW, idle_s=45)
    assert fresh["dim"] == 100
    assert dimmed["dim"] == 20


def test_no_frame_value_repeats_free_text_from_the_state() -> None:
    """Redaction by construction: text is composed from identifiers, never copied.

    A frame that cannot contain log text cannot leak one, which is what keeps
    architecture §8 out of the device path entirely.
    """
    state = _state()
    free_text = {
        str(f.get("title", "")) for f in state.get("findings") or []
    } | {str(state.get("command", ""))}
    free_text = {t for t in free_text if len(t) > 8}
    for want in ALL_WANTS:
        blob = frames.encode(project.project(state, devices.CORE2, want)).decode()
        for text in free_text:
            assert text not in blob, f"{text!r} was copied into {devices.WANT_NAMES[want]}"


# ── M5-004: the numbers, against the recorded run ────────────────────────────


@real_run_only
def test_velocity_reproduces_the_measured_run() -> None:
    """``15 7 2 2 6 4 6`` — the stall between 1.5 and 2 hours that no other view shows.

    The leading zero is real and kept: the opening half-hour closes nothing, because
    generate and upload produce no closures. It is the same fact BURNDOWN's flat first
    four points show.
    """
    assert stats.velocity_buckets(_real()) == [0, 15, 7, 2, 2, 6, 4, 6]


@real_run_only
def test_analytics_reproduces_the_measured_shares() -> None:
    table = stats.step_table(_real())
    assert [row[2] for row in table] == [41, 26, 16, 10, 7]
    assert stats.coverage_pct(_real()) == 42


@real_run_only
def test_analytics_is_unmoved_by_an_unclosed_node() -> None:
    """The property that makes the screen survivable on damaged data.

    A sum is distorted by one orphan; a median is not. This is why ANALYTICS did not
    have to wait for the instrumentation to be fixed.
    """
    state = _real()
    before = stats.step_table(state)
    version = next(cast(dict[str, Any], n) for n in stats.walk(state["tree"])
                   if n["kind"] == "version")
    version.setdefault("children", []).append({
        "id": "execute-issues", "kind": "step", "status": "running",
        "start": state["started"], "end": None, "elapsed_s": 9300.0,
    })
    assert stats.step_table(state) == before


@real_run_only
def test_burndown_never_rises() -> None:
    series = stats.burndown(_real())
    assert series == sorted(series, reverse=True)
    assert series[0] == stats.total_issues(_real())


@pytest.mark.parametrize("preset", ["clean-run", "retry-run", "aborted-run", "skipped-versions"])
def test_burndown_never_rises_on_any_fixture(preset: str) -> None:
    series = stats.burndown(_state(preset))
    assert series == sorted(series, reverse=True)


@real_run_only
def test_the_now_label_is_not_read_from_state_current() -> None:
    """``state.current`` reduces to the step name three times, with no version and no
    issue (vision §9.4). The tree still has the truth; only the derived field is
    degenerate, so the projection walks the tree instead."""
    state = _real()
    assert state["current"] == "execute-issues · execute-issues · execute-issues"
    label, step = stats.running_label(state)
    assert label == "v05.03 RF-112"
    assert step == "execute-issues"


@real_run_only
def test_the_estimate_gap_is_reported_rather_than_hidden() -> None:
    """58 predicted, 42 delivered. The gap is the measurement, which is why the line
    is labelled *estimate* and never *ideal* (vision §2.4)."""
    state = _real()
    assert stats.estimated_issues(state) == 58
    assert stats.total_issues(state) == 42
    assert stats.estimate_error_pct(state) == 38


@real_run_only
def test_the_median_is_used_rather_than_the_mean() -> None:
    """``metrics.mean_issue_s`` reports 631 s against a real median near 140, because
    the mean is dragged by a long tail. The pill compares against the median."""
    state = _real()
    median = stats.issue_median_s(state)
    assert median is not None
    assert median < float(state["metrics"]["mean_issue_s"]) / 3


@real_run_only
def test_friction_reproduces_the_measured_retries_and_findings() -> None:
    state = _real()
    assert stats.retried_issues(state)[:2] == [("RF-086", 4), ("RF-084", 3)]
    assert stats.findings_by_severity(state) == [0, 6, 3]


@real_run_only
@pytest.mark.parametrize("want", ALL_WANTS, ids=IDS)
def test_every_screen_of_the_real_run_fits_one_write(want: int) -> None:
    frame = project.project(_real(), devices.CORE2, want)
    assert frames.validate(frame) == []


# ── the age pill ─────────────────────────────────────────────────────────────


@pytest.mark.parametrize(
    ("age", "expected"),
    [(0, stats.CC_OK), (100, stats.CC_OK), (300, stats.CC_WARN), (600, stats.CC_ALARM)],
)
def test_the_age_class_is_a_multiple_of_the_median_not_a_fixed_number(
    age: float, expected: int
) -> None:
    """Thresholds scale, so a faster or slower project needs no retuning."""
    assert stats.issue_age_class(age, median_s=100) == expected


def test_the_age_class_is_calm_when_nothing_is_running() -> None:
    assert stats.issue_age_class(None, 100) == stats.CC_OK
    assert stats.issue_age_class(500, None) == stats.CC_OK


# ── sparkline encoding ───────────────────────────────────────────────────────


def test_a_sparkline_is_digits_not_glyphs() -> None:
    """The frame carries numbers; the firmware draws rectangles. This is what means
    no glyph set has to ship, and no empty box can appear on the panel."""
    assert stats.spark([0, 15, 7, 2]) == "0731"
    assert stats.spark([]) == ""
    assert stats.spark([0, 0]) == "00"
    assert all(c.isdigit() for c in stats.spark([3, 9, 1]))


def test_a_json_encoded_frame_is_what_the_size_check_measures() -> None:
    """Compact separators, so the guard measures the bytes that actually travel."""
    blob = frames.encode({"a": 1, "b": [2, 3]})
    assert blob == b'{"a":1,"b":[2,3]}'
    assert json.loads(blob)

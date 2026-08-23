"""M5-005 — golden frames. Freeze the output so later refactors are safe.

The equivalent of TRK-008 for the device. The value is not in any one frame but in the
diff: a change to a projection that nobody intended shows up as a named field with a
before and an after, rather than as a panel that looks slightly wrong on a desk.

Built from presets rather than from ``runs/``, which is gitignored — a golden frame
derived from a recorded run would be unreproducible in a fresh checkout.
"""

from __future__ import annotations

import json
from typing import Any

import pytest

from bridge import devices, frames, stats
from bridge.devices import Profile
from tests import gen_frames

TARGETS = gen_frames.targets()
IDS = [f"{p.name}-{devices.WANT_NAMES[w]}" for p, w in TARGETS]


@pytest.mark.parametrize(("profile", "want"), TARGETS, ids=IDS)
def test_every_golden_frame_is_committed(profile: Profile, want: int) -> None:
    path = gen_frames.path_for(profile, want)
    assert path.is_file(), "missing golden: run python3 -m tests.gen_frames --update-golden"


@pytest.mark.parametrize(("profile", "want"), TARGETS, ids=IDS)
def test_every_golden_frame_reproduces_exactly(profile: Profile, want: int) -> None:
    """Byte-for-byte after a round trip through JSON, not merely equal-ish."""
    expected = json.loads(gen_frames.path_for(profile, want).read_text())
    assert gen_frames.build(profile, want) == expected


@pytest.mark.parametrize(("profile", "want"), TARGETS, ids=IDS)
def test_every_golden_frame_passes_both_guards(profile: Profile, want: int) -> None:
    frame = json.loads(gen_frames.path_for(profile, want).read_text())
    assert frames.validate(frame) == []


def test_there_are_nine_goldens_not_fourteen() -> None:
    """Corrected from the plan, which assumed seven screens on both boards.

    The StickC renders NOW and the notification channel only, so it has no burndown
    frame to have. Seven plus two, not seven times two.
    """
    assert len(TARGETS) == 9
    core2 = [t for t in TARGETS if t[0] is devices.CORE2]
    stickc = [t for t in TARGETS if t[0] is devices.STICKC]
    assert len(core2) == 7
    assert len(stickc) == 2


def test_a_changed_projection_fails_with_the_field_named() -> None:
    """The diff is the product. A golden that failed with "not equal" would send you
    reading two files; this one names what moved."""
    profile, want = devices.CORE2, devices.SCREEN_NOW
    expected = json.loads(gen_frames.path_for(profile, want).read_text())
    tampered = {**expected, "pct": expected["pct"] + 1}
    changed = [k for k in tampered if tampered[k] != expected.get(k)]
    assert changed == ["pct"]


def test_regeneration_is_refused_without_the_explicit_flag() -> None:
    """A golden file that rewrites itself asserts nothing."""
    assert gen_frames.main([]) == 2
    assert gen_frames.main(["--something-else"]) == 2


def test_the_generator_builds_with_an_empty_runs_directory() -> None:
    """``runs/`` is gitignored, so a golden built from one would not survive a clone.

    Behavioural, not textual: an earlier version of this test grepped the source for
    the word "runs" and failed on the docstring explaining why it is not read. What
    matters is that generation works with nothing recorded — which the autouse fixture
    in ``conftest.py`` already guarantees by pointing the runs root at an empty
    ``tmp_path`` for every test in this file.
    """
    from tracker import paths

    assert not list(paths.runs_root().glob("run-*")), "fixture isolation lost"
    frame = gen_frames.build(devices.CORE2, devices.SCREEN_BURNDOWN)
    assert frames.validate(frame) == []


# ── coverage: what it actually measures ──────────────────────────────────────


def test_coverage_rises_when_steps_close_and_falls_when_they_do_not() -> None:
    """Inherited from M5-004, and **its criterion was wrong**.

    It asked for ``cov == 100`` on a log with no unclosed step nodes. That can never
    happen: coverage is closed-span time over run time, and steps do not tile a run —
    there is always time between them. A clean preset with every step closed reaches
    91%, not 100.

    What is worth asserting is the contrast. Coverage is high when steps close and
    collapses when they do not, which is exactly the emission gap the badge exists to
    report — and exactly what the reducer fix for #113 deliberately did not change.
    """
    complete = gen_frames.state()
    assert stats.coverage_pct(complete) > 85

    stripped: dict[str, Any] = json.loads(json.dumps(complete))
    _open_every_step(stripped)
    assert stats.coverage_pct(stripped) == 0


def _open_every_step(state: dict[str, Any]) -> None:
    """Reopen every step, as a run that stopped emitting ``step.end`` would leave it."""

    def walk(nodes: list[dict[str, Any]]) -> None:
        for node in nodes:
            if node.get("kind") == "step":
                node["end"] = None
                node["status"] = "running"
            walk(node.get("children") or [])

    walk(state["tree"])

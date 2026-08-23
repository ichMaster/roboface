"""M5-006 — the notification catalogue and its queue.

The catalogue is checked for *completeness*, not for the counts one run happened to
produce. Three kinds scored zero in the measured run; they are the ones that matter
most when they fire, so a test that only covered what happened would cover the wrong
thing.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from typing import Any

import pytest

from bridge import devices, frames, notify
from tests import gen_log
from tracker import reduce as reduce_mod

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)


def _state(preset: str = "clean-run", upto: int | None = None) -> dict[str, Any]:
    lines = gen_log.preset(preset).splitlines()
    return reduce_mod.reduce(lines[:upto] if upto else lines, NOW).as_dict()


# ── the catalogue ────────────────────────────────────────────────────────────


def test_every_volume_level_is_used() -> None:
    """A scale with an unused rung is a rung nobody tuned."""
    used = {volume for volume, _ in notify.CATALOGUE.values()}
    assert used == {notify.SILENT, notify.CHIME, notify.SHORT, notify.LONG}


@pytest.mark.parametrize("kind", ["failed", "held", "blocked"])
def test_the_kinds_that_scored_zero_are_still_in_the_catalogue(kind: str) -> None:
    """Nothing failed in the measured run. That is not a reason to drop the entry —
    the same argument the FRICTION screen rests on: empty is not unnecessary."""
    volume, goto = notify.CATALOGUE[kind]
    assert volume == notify.LONG
    assert goto is not None


def test_silent_events_still_carry_no_goto() -> None:
    """A release is worth knowing and not worth interrupting for."""
    assert notify.CATALOGUE["release"] == (notify.SILENT, None)


def test_a_notification_renders_to_a_valid_frame_item() -> None:
    item = notify.Notification("retry", "RF-086 x4").as_item(devices.CORE2)
    assert item == {"k": "retry", "t": "RF-086 x4", "b": 2, "g": devices.SCREEN_FRICTION}


def test_goto_is_omitted_when_the_board_cannot_show_that_screen() -> None:
    """Telling a StickC to jump to FRICTION would leave it asking for a frame nobody
    answers."""
    item = notify.Notification("retry", "RF-086 x4").as_item(devices.STICKC)
    assert "g" not in item


def test_text_is_truncated_to_the_board() -> None:
    long = notify.Notification("retry", "A" * 80).as_item(devices.STICKC)
    assert len(long["t"]) <= devices.STICKC.chars_per_line


# ── the diff, which is how transitions are recovered at all ──────────────────


def test_the_first_snapshot_raises_nothing() -> None:
    """A device connecting mid-run must not be buzzed for every version that finished
    before it arrived."""
    queue = notify.Queue()
    assert queue.observe(_state()) == []
    assert len(queue) == 0


def test_a_finishing_run_raises_a_chime() -> None:
    queue = notify.Queue()
    running = {**_state(), "status": "running"}
    queue.observe(running)
    raised = queue.observe({**running, "status": "done"})
    assert [n.kind for n in raised] == ["run.end"]


def test_a_version_completing_raises_both_the_end_and_the_release() -> None:
    """Two facts, and the second is the one you wait for."""
    before = _state()
    versions = [n for n in _walk(before) if n["kind"] == "version"]
    versions[-1]["status"] = "running"
    after = json.loads(json.dumps(before))
    [n for n in _walk(after) if n["kind"] == "version"][-1]["status"] = "ok"

    queue = notify.Queue()
    queue.observe(before)
    kinds = [n.kind for n in queue.observe(after)]
    assert "version.end" in kinds and "release" in kinds


def test_a_retry_is_raised_once_per_new_attempt() -> None:
    before = _state()
    issues = [n for n in _walk(before) if n["kind"] == "issue"]
    issues[0].setdefault("data", {})["attempts"] = 1
    after = json.loads(json.dumps(before))
    [n for n in _walk(after) if n["kind"] == "issue"][0]["data"]["attempts"] = 2

    queue = notify.Queue()
    queue.observe(before)
    raised = queue.observe(after)
    assert [(n.kind, n.text) for n in raised if n.kind == "retry"]

    # Same state again: the attempt has not changed, so nothing new is raised.
    assert [n for n in queue.observe(after) if n.kind == "retry"] == []


def test_a_new_finding_is_raised_and_an_existing_one_is_not() -> None:
    before = {**_state(), "findings": [{"id": "1", "version": "v01.01", "severity": "LOW"}]}
    after = {
        **before,
        "findings": [
            {"id": "1", "version": "v01.01", "severity": "LOW"},
            {"id": "2", "version": "v01.02", "severity": "MEDIUM"},
        ],
    }
    queue = notify.Queue()
    queue.observe(before)
    raised = queue.observe(after)
    assert [n.text for n in raised] == ["v01.02 MEDIUM"]


def test_a_held_finding_is_louder_than_a_raised_one() -> None:
    before = {**_state(), "findings": []}
    after = {**before, "findings": [{"id": "9", "version": "v02.01",
                                     "severity": "HIGH", "outcome": "held"}]}
    queue = notify.Queue()
    queue.observe(before)
    raised = queue.observe(after)
    assert raised[0].kind == "held"
    assert notify.CATALOGUE[raised[0].kind][0] == notify.LONG


def test_an_unchanged_state_raises_nothing() -> None:
    """The common case by a wide margin: 57 notifications across 4.1 hours."""
    state = _state()
    queue = notify.Queue()
    queue.observe(state)
    assert queue.observe(state) == []
    assert queue.observe(state) == []


# ── the queue ────────────────────────────────────────────────────────────────


def test_a_burst_of_five_leaves_three_then_two_in_order() -> None:
    queue = notify.Queue()
    queue._pending = [notify.Notification("release", f"v{i}") for i in range(5)]
    first = queue.take(devices.CORE2)
    second = queue.take(devices.CORE2)
    assert [i["t"] for i in first] == ["v0", "v1", "v2"]
    assert [i["t"] for i in second] == ["v3", "v4"]
    assert len(queue) == 0


def test_taken_notifications_never_reappear() -> None:
    """Dropped once answered. A lost write costs one buzz — the buzz is the
    notification, the screen is the record."""
    queue = notify.Queue()
    queue._pending = [notify.Notification("retry", "RF-001 x2")]
    assert queue.take(devices.CORE2)
    assert queue.take(devices.CORE2) == []


def test_a_full_batch_still_fits_one_write() -> None:
    queue = notify.Queue()
    queue._pending = [
        notify.Notification("retry", "RF-086 x4"),
        notify.Notification("finding", "v01.02 MEDIUM"),
        notify.Notification("release", "v05.03 tagged"),
    ]
    frame = {
        "v": 1, "s": 0, "next": 5, "dim": 100,
        "n": queue.take(devices.CORE2),
    }
    assert frames.validate(frame) == []


@pytest.mark.parametrize("kind", list(notify.CATALOGUE))
def test_every_catalogue_entry_produces_an_ascii_item(kind: str) -> None:
    item = notify.Notification(kind, "v01.02 RF-007 x2").as_item(devices.CORE2)
    assert json.dumps(item).isascii()


def _walk(state: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []

    def go(nodes: list[dict[str, Any]]) -> None:
        for node in nodes:
            out.append(node)
            go(node.get("children") or [])

    go(state["tree"])
    return out

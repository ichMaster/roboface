"""M5-007 / M5-008 — pacing, the brightness ladder, and navigation.

All three are bridge policy, so all three are ``pytest`` rather than a thing to confirm
by watching a panel. ``now`` is a parameter, so none of it waits for real time.
"""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Any

import pytest

from bridge import devices, frames, notify, session
from tests import gen_log
from tracker import reduce as reduce_mod

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)


def _state(status: str = "running") -> dict[str, Any]:
    state = reduce_mod.reduce(gen_log.preset("clean-run").splitlines(), NOW).as_dict()
    state["status"] = status
    return state


def _session(profile: devices.Profile = devices.CORE2, status: str = "running") -> session.Session:
    sess = session.Session(profile)
    sess.observe(_state(status))
    return sess


# ── M5-007: pacing ───────────────────────────────────────────────────────────


def test_a_running_run_uses_the_per_screen_intervals() -> None:
    sess = _session()
    assert sess.answer(devices.SCREEN_NOW, now=0)["next"] == 15
    assert sess.answer(devices.SCREEN_VELOCITY, now=0)["next"] == 120
    assert sess.answer(devices.SCREEN_PLAN, now=0)["next"] == 60


def test_a_finished_run_slows_every_screen_down() -> None:
    """Nothing will change again, and a device may sit on a desk for days."""
    sess = _session(status="done")
    for screen in devices.CORE2.screens:
        assert sess.answer(screen, now=0)["next"] == session.IDLE_POLL_S


def test_notifications_keep_their_rate_even_when_the_run_has_ended() -> None:
    """A finished run still has a last chime to deliver."""
    sess = _session(status="done")
    assert sess.answer(devices.WANT_NOTIFY, now=0)["next"] == 5


# ── M5-007: the ladder ───────────────────────────────────────────────────────


@pytest.mark.parametrize(("idle", "expected"), [(0, 100), (29, 100), (30, 50), (45, 20), (600, 20)])
def test_the_core2_ladder_steps_at_two_and_three_times_the_now_interval(
    idle: float, expected: int
) -> None:
    sess = _session()
    assert sess.answer(devices.SCREEN_NOW, now=idle)["dim"] == expected


def test_the_core2_never_reaches_zero_and_the_stickc_does() -> None:
    """The §1.1 split: one board is read, the other is felt."""
    core2 = _session(devices.CORE2)
    stickc = _session(devices.STICKC)
    assert core2.answer(devices.SCREEN_NOW, now=10_000)["dim"] == 20
    assert stickc.answer(devices.SCREEN_NOW, now=10_000)["dim"] == 0


def test_an_interaction_restores_full_brightness_and_a_scheduled_poll_does_not() -> None:
    """Otherwise the panel stays lit forever simply because it keeps asking."""
    sess = _session()
    assert sess.answer(devices.SCREEN_NOW, now=60)["dim"] == 20
    assert sess.answer(devices.SCREEN_NOW, now=61, user=True)["dim"] == 100
    assert sess.answer(devices.SCREEN_NOW, now=91)["dim"] == 50


def test_retuning_the_now_interval_moves_the_ladder_through_the_session() -> None:
    """The derivation survives the whole call path, not just ``Profile.dim_at``."""
    import dataclasses

    slower = dataclasses.replace(
        devices.CORE2, poll_s={**devices.CORE2.poll_s, devices.SCREEN_NOW: 30}
    )
    sess = _session(slower)
    assert sess.answer(devices.SCREEN_NOW, now=30)["dim"] == 100
    assert sess.answer(devices.SCREEN_NOW, now=60)["dim"] == 50


def test_every_answer_carries_both_pacing_fields() -> None:
    sess = _session()
    for want in (devices.WANT_NOTIFY, *devices.CORE2.screens):
        frame = sess.answer(want, now=0)
        assert "next" in frame and "dim" in frame
        assert frames.validate(frame) == []


# ── M5-008: navigation ───────────────────────────────────────────────────────


def test_an_alert_carries_the_device_to_the_relevant_screen() -> None:
    sess = _session()
    sess.queue._pending = [notify.Notification("retry", "RF-007 x2")]
    frame = sess.answer(devices.WANT_NOTIFY, now=0)
    assert frame["g"] == devices.SCREEN_FRICTION
    assert sess.screen == devices.SCREEN_FRICTION


def test_a_silent_event_does_not_move_the_device() -> None:
    sess = _session()
    sess.queue._pending = [notify.Notification("release", "v01.02 tagged")]
    assert "g" not in sess.answer(devices.WANT_NOTIFY, now=0)


def test_the_device_goes_home_thirty_seconds_after_the_last_interaction() -> None:
    sess = _session()
    sess.answer(devices.SCREEN_PLAN, now=0, user=True)
    assert "g" not in sess.answer(devices.SCREEN_PLAN, now=29)
    assert sess.answer(devices.SCREEN_PLAN, now=30)["g"] == devices.SCREEN_NOW


def test_the_device_already_home_is_not_told_to_go_home() -> None:
    """``g`` is absent, not null, when there is nothing to navigate to."""
    sess = _session()
    assert "g" not in sess.answer(devices.SCREEN_NOW, now=10_000)


def test_an_alert_beats_the_return_timer() -> None:
    """Something just happened, which is worth more than a timer."""
    sess = _session()
    sess.answer(devices.SCREEN_PLAN, now=0, user=True)
    sess.queue._pending = [notify.Notification("failed", "RF-007 failed")]
    assert sess.answer(devices.WANT_NOTIFY, now=999)["g"] == devices.SCREEN_FRICTION


def test_pressing_a_button_selects_that_screen() -> None:
    sess = _session()
    sess.answer(devices.SCREEN_BURNDOWN, now=0, user=True)
    assert sess.screen == devices.SCREEN_BURNDOWN


def test_polling_notifications_does_not_change_which_screen_is_showing() -> None:
    """The notification channel is not a screen."""
    sess = _session()
    sess.answer(devices.SCREEN_PLAN, now=0, user=True)
    sess.answer(devices.WANT_NOTIFY, now=1, user=True)
    assert sess.screen == devices.SCREEN_PLAN

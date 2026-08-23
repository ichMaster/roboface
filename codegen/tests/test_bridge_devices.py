"""M5-001 — device profiles.

A profile is data. The tests that matter are the ones proving it stays data: that the
two boards genuinely differ (so a copy-paste profile fails), and that the brightness
ladder is *derived* from the NOW poll interval rather than written as constants — the
property that makes retuning that interval safe (vision §4.4).
"""

from __future__ import annotations

import dataclasses

import pytest

from bridge import devices
from bridge.devices import CORE2, PROFILES, STICKC, Profile


def test_the_two_boards_are_not_the_same_object_with_a_different_name() -> None:
    """A copy-paste profile is the likely mistake; this is what catches it."""
    assert (CORE2.width, CORE2.height) != (STICKC.width, STICKC.height)
    assert CORE2.screens != STICKC.screens
    assert CORE2.dim_ladder != STICKC.dim_ladder
    assert CORE2.chars_per_line != STICKC.chars_per_line


def test_core2_has_six_screens_and_stickc_has_one() -> None:
    assert len(CORE2.screens) == 6
    assert STICKC.screens == (devices.SCREEN_NOW,)


def test_every_screen_a_board_claims_has_a_poll_interval() -> None:
    """A screen with no interval would leave the device with nothing to wait for."""
    for profile in PROFILES.values():
        for want in (devices.WANT_NOTIFY, *profile.screens):
            assert profile.poll_for(want) > 0


def test_notifications_are_polled_faster_than_any_screen() -> None:
    """It is the only channel that can buzz, so it sets the one real latency need."""
    for profile in PROFILES.values():
        slowest_notify = profile.poll_for(devices.WANT_NOTIFY)
        assert all(slowest_notify <= profile.poll_for(s) for s in profile.screens)


# ── the ladder is derived, which is the whole point ──────────────────────────


def test_the_core2_dims_but_never_goes_dark() -> None:
    """Vision §1 claims the panel is always visible; this is what keeps that true."""
    assert CORE2.dim_at(0) == 100
    assert CORE2.dim_at(30) == 50
    assert CORE2.dim_at(45) == 20
    assert CORE2.dim_at(10_000) == 20
    assert 0 not in CORE2.dim_ladder


def test_the_stickc_goes_dark_because_it_is_a_pager() -> None:
    assert STICKC.dim_at(0) == 100
    assert STICKC.dim_at(30) == 0


def test_retuning_the_now_interval_moves_the_whole_ladder() -> None:
    """The property a pair of hardcoded thresholds would not have.

    Doubling the NOW interval must double both steps with no other edit — otherwise
    the ladder silently becomes wrong the first time the interval is tuned.
    """
    slower = dataclasses.replace(
        CORE2, poll_s={**CORE2.poll_s, devices.SCREEN_NOW: 30}
    )
    assert slower.dim_at(30) == 100, "was a step at 30 s; must no longer be"
    assert slower.dim_at(60) == 50
    assert slower.dim_at(90) == 20


@pytest.mark.parametrize("profile", list(PROFILES.values()), ids=lambda p: p.name)
def test_a_ladder_never_brightens_as_time_passes(profile: Profile) -> None:
    levels = [profile.dim_at(t) for t in range(0, 200, 5)]
    assert levels == sorted(levels, reverse=True)


# ── the roster ───────────────────────────────────────────────────────────────


def test_has_covers_the_notification_channel_for_every_board() -> None:
    """``want:0`` is not a screen, so ``screens`` does not list it — but every board
    answers it, and a board that did not would never buzz."""
    for profile in PROFILES.values():
        assert profile.has(devices.WANT_NOTIFY)


def test_the_stickc_does_not_claim_screens_it_cannot_draw() -> None:
    assert not STICKC.has(devices.SCREEN_BURNDOWN)
    assert CORE2.has(devices.SCREEN_BURNDOWN)


def test_profiles_are_keyed_by_their_own_name() -> None:
    assert all(name == profile.name for name, profile in PROFILES.items())


def test_profiles_are_frozen() -> None:
    """Shared, module-level, and read by every projection — mutating one mid-run would
    change what a device is halfway through answering it."""
    with pytest.raises(dataclasses.FrozenInstanceError):
        CORE2.name = "nope"  # type: ignore[misc]


def test_every_want_has_a_name() -> None:
    for profile in PROFILES.values():
        for want in (devices.WANT_NOTIFY, *profile.screens):
            assert devices.WANT_NAMES[want]

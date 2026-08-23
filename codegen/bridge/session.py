"""One device's session — pacing, brightness, navigation.

Everything the device would otherwise need a timer or a memory for lives here, which
is the whole of vision §3.1: the panel obeys the last numbers it was given and holds
nothing between frames.

Like ``project()``, this takes ``now`` as a parameter rather than reading a clock, so
every behaviour below — the ladder, the thirty-second return, the pacing change when a
run ends — is testable without waiting for real time to pass.

**The device flags user action.** A poll carries ``{"want":N,"u":1}`` when a button or
a tap caused it. The vision doc says the bridge may read any inbound ``want`` as *the
user is present*, but that cannot distinguish a tap on the current screen from the
scheduled poll for that same screen — and M5-007 requires exactly that distinction. One
optional field is cheaper than guessing.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from bridge import devices, notify, project
from bridge.devices import Profile

#: Seconds of no interaction after which the bridge sends the device back to NOW.
RETURN_AFTER_S = 30.0

#: Pace for a run that has ended, or when there is no run at all. Nothing will change
#: again, and a device may sit on a desk for days — polling every fifteen seconds
#: forever is pure waste.
IDLE_POLL_S = 60


class Session:
    """Per-device state. One of these per board in the roster."""

    def __init__(self, profile: Profile, *, started_at: float = 0.0) -> None:
        self.profile = profile
        self.queue = notify.Queue()
        self.screen = devices.SCREEN_NOW
        self.last_interaction_at = started_at
        self._state: dict[str, Any] = {}

    # ── inputs ───────────────────────────────────────────────────────────────

    def observe(self, state: Mapping[str, Any]) -> list[notify.Notification]:
        """Take a fresh snapshot from the dashboard."""
        self._state = dict(state)
        return self.queue.observe(state)

    def answer(self, want: int, now: float, *, user: bool = False) -> dict[str, Any]:
        """The frame for this poll.

        ``user`` marks a press or a tap. It restarts the brightness ladder and the
        return timer; a scheduled poll does neither, which is what stops the panel
        from staying lit forever simply because it keeps asking questions.
        """
        if user:
            self.last_interaction_at = now
            if want != devices.WANT_NOTIFY:
                self.screen = want

        idle = max(0.0, now - self.last_interaction_at)
        frame = project.project(
            self._state,
            self.profile,
            want,
            idle_s=idle,
            notifications=self.queue.take(self.profile) if want == devices.WANT_NOTIFY else None,
        )
        frame["next"] = self._pace(want)

        goto = self._goto(frame, now)
        if goto is not None:
            frame["g"] = goto
            self.screen = goto
        return frame

    # ── the two policies ─────────────────────────────────────────────────────

    def _pace(self, want: int) -> int:
        """How long the device should wait before asking again.

        Notifications keep their rate whatever the run is doing: they are the channel
        that can buzz, and a finished run still has a last chime to deliver.
        """
        if want == devices.WANT_NOTIFY:
            return self.profile.poll_for(want)
        if self._finished():
            return IDLE_POLL_S
        return self.profile.poll_for(want)

    def _goto(self, frame: Mapping[str, Any], now: float) -> int | None:
        """Which screen the device should switch to, if any.

        An alert's own ``g`` wins: something just happened, and it is worth more than
        a timer. Otherwise, thirty seconds after the last interaction the device goes
        home — so the resting state is always the same screen.
        """
        for item in frame.get("n") or []:
            target = item.get("g")
            if isinstance(target, int) and target != self.screen:
                return target

        if self.screen != devices.SCREEN_NOW and now - self.last_interaction_at >= RETURN_AFTER_S:
            return devices.SCREEN_NOW
        return None

    def _finished(self) -> bool:
        return str(self._state.get("status", "")) not in {"running", ""}

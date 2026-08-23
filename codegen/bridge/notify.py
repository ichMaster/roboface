"""Notifications — what *happened*, as against the screens, which show what *is*.

Alerts and events are one list. The only thing separating them is ``b``, the volume,
and ``b`` governs sound and haptics only — never the backlight. Every notification
lights the screen, including a silent one (vision §5.1, §4.4).

**Derived by diffing successive states, not by reading events.** The plan assumed the
bridge would map log events to notifications; it cannot. ``dashboard/server.py`` sends
``{"kind": ..., "state": ...}`` and no event, so the only thing the bridge ever sees is
a snapshot. Transitions are therefore recovered by comparing the previous snapshot with
the current one — which the bridge is free to do, since only the *device* is forbidden
state.

The queue is dropped once answered. A lost write costs one buzz, which is accepted:
**the buzz is the notification and the screen is the record.** FRICTION still shows the
retry whenever you look. The alternative — acknowledgement plus dedupe by id — would
put comparison-against-previous back on the device, which vision §3.1 exists to prevent.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

from bridge import devices, frames, stats

#: Volume levels. Not categories — one scale from silent to insistent.
SILENT, CHIME, SHORT, LONG = 0, 1, 2, 3

#: Kind → volume, and the screen to jump to. A kind with no screen carries no ``g``,
#: which is absent rather than null so it costs no bytes.
#:
#: ``issue.failed``, ``harden.finding.held`` and ``gate.blocked`` scored zero in the
#: measured run. That is not a reason to drop them: they are the notifications that
#: matter most when they do happen, exactly the argument the FRICTION screen rests on.
CATALOGUE: Mapping[str, tuple[int, int | None]] = {
    "run.end": (CHIME, devices.SCREEN_NOW),
    "phase.end": (CHIME, None),
    "release": (SILENT, None),
    "version.end": (SILENT, None),
    "retry": (SHORT, devices.SCREEN_FRICTION),
    "finding": (SHORT, devices.SCREEN_FRICTION),
    "failed": (LONG, devices.SCREEN_FRICTION),
    "held": (LONG, devices.SCREEN_FRICTION),
    "blocked": (LONG, devices.SCREEN_NOW),
}


@dataclass(frozen=True)
class Notification:
    """One thing that happened, ready to become a frame entry."""

    kind: str
    text: str

    def as_item(self, profile: devices.Profile) -> dict[str, Any]:
        volume, goto = CATALOGUE.get(self.kind, (SILENT, None))
        item: dict[str, Any] = {
            "k": self.kind,
            "t": _fit(self.text, profile),
            "b": volume,
        }
        # Only when the board can actually show that screen — telling a StickC to jump
        # to BURNDOWN would leave it asking for a frame nobody will answer.
        if goto is not None and profile.has(goto):
            item["g"] = goto
        return item


class Queue:
    """Holds notifications until a poll takes them.

    At most :data:`frames.MAX_NOTIFICATIONS` leave per answer, oldest first; the rest
    wait. With 57 notifications across 4.1 hours against a five-second poll, the queue
    is almost always empty and a burst of more than three is close to impossible — but
    the carry-over costs nothing and removes the question.
    """

    def __init__(self) -> None:
        self._pending: list[Notification] = []
        self._previous: dict[str, Any] | None = None

    def __len__(self) -> int:
        return len(self._pending)

    def observe(self, state: Mapping[str, Any]) -> list[Notification]:
        """Diff against the last snapshot and enqueue whatever changed.

        The first snapshot raises nothing: a device connecting mid-run must not be
        buzzed for every version that finished before it arrived.
        """
        current = dict(state)
        raised = [] if self._previous is None else diff(self._previous, current)
        self._previous = current
        self._pending.extend(raised)
        return raised

    def take(self, profile: devices.Profile) -> list[dict[str, Any]]:
        """Up to three items, removed from the queue. Dropped once handed over."""
        batch = self._pending[: frames.MAX_NOTIFICATIONS]
        self._pending = self._pending[frames.MAX_NOTIFICATIONS :]
        return [n.as_item(profile) for n in batch]

    def peek(self) -> list[Notification]:
        return list(self._pending)


def diff(before: Mapping[str, Any], after: Mapping[str, Any]) -> list[Notification]:
    """Notifications implied by the change from one snapshot to the next."""
    raised: list[Notification] = []

    if before.get("status") == "running" and after.get("status") != "running":
        raised.append(Notification("run.end", f"run {after.get('status', 'ended')}"))

    old_nodes = _index(before)
    for key, node in _index(after).items():
        kind, name = key
        was = old_nodes.get(key, {})
        raised.extend(_node_transition(kind, name, was, node))

    for note in _finding_transitions(before, after):
        raised.append(note)

    return raised


def _node_transition(
    kind: str, name: str, was: Mapping[str, Any], now: Mapping[str, Any]
) -> list[Notification]:
    # Attempts first: a retry changes ``data.attempts`` and leaves ``status`` alone, so
    # checking it after the status guard below made it unreachable. Found by the test.
    attempts = int((now.get("data") or {}).get("attempts") or 1)
    was_attempts = int((was.get("data") or {}).get("attempts") or 1)
    if kind == "issue" and attempts > 1 and attempts != was_attempts:
        return [Notification("retry", f"{name} x{attempts}")]

    before_status, after_status = was.get("status"), now.get("status")
    if before_status == after_status:
        return []

    if after_status == "ok" and kind in {"phase", "version"}:
        note = [Notification(f"{kind}.end", f"{name} done")]
        if kind == "version":
            # A released version is two facts, and the second is the one you wait for.
            note.append(Notification("release", f"{name} tagged"))
        return note
    if after_status in {"fail", "aborted"} and kind == "issue":
        return [Notification("failed", f"{name} failed")]
    if after_status in {"fail", "aborted"} and kind == "step":
        return [Notification("blocked", f"{name} blocked")]
    return []


def _finding_transitions(
    before: Mapping[str, Any], after: Mapping[str, Any]
) -> list[Notification]:
    seen = {str(f.get("id")) for f in before.get("findings") or []}
    raised = []
    for finding in after.get("findings") or []:
        if str(finding.get("id")) in seen:
            continue
        severity = str(finding.get("severity", "")).upper()
        version = str(finding.get("version", ""))
        kind = "held" if finding.get("outcome") == "held" else "finding"
        raised.append(Notification(kind, f"{version} {severity}".strip()))
    return raised


def _index(state: Mapping[str, Any]) -> dict[tuple[str, str], Mapping[str, Any]]:
    return {
        (str(n.get("kind")), str(n.get("id"))): n
        for n in stats.walk(state.get("tree") or [])
        if n.get("kind") in {"phase", "version", "step", "issue"}
    }


def _fit(text: str, profile: devices.Profile) -> str:
    limit = profile.chars_per_line
    return text if len(text) <= limit else text[: limit - 1] + "."

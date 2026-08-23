"""``(state, profile, screen) -> frame``. Pure, and the only place a device value is decided.

Purity is the testability lever, exactly as it is for ``tracker.reduce``: no clock, no
I/O, no environment. Everything time-dependent — how long the device has been idle, how
long until it should ask again — arrives as an argument.

**Text is composed from identifiers, never copied from the log.** ``"RF-086 x4"`` is
built from an issue id and a count; it is never an issue title, a commit message or a
finding's description. That is what keeps redaction (architecture §8) out of the device
path entirely: a frame that cannot contain log text cannot leak one.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from bridge import devices, frames, stats
from bridge.devices import Profile


def project(
    state: Mapping[str, Any],
    profile: Profile,
    screen: int,
    *,
    idle_s: float = 0.0,
    notifications: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """One screen's frame, ready to write.

    ``idle_s`` drives the brightness ladder and ``profile`` drives everything else, so
    the same state yields different bytes per board — deliberately (vision §3.2). An
    earlier draft claimed both boards received identical frames and offered that as
    proof the contract was right; formatting for a screen width retired that claim.
    """
    if not profile.has(screen):
        raise ValueError(f"{profile.name} does not render screen {screen}")

    frame: dict[str, Any] = {
        "v": frames.FRAME_VERSION,
        "s": screen,
        "next": profile.poll_for(screen),
        "dim": profile.dim_at(idle_s),
    }
    if screen == devices.WANT_NOTIFY:
        frame["n"] = list(notifications or [])[: frames.MAX_NOTIFICATIONS]
        return frame

    frame.update(_BUILDERS[screen](state, profile))
    return frame


# ── per-screen bodies ────────────────────────────────────────────────────────


def _now(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    label, step = stats.running_label(state)
    age = stats.current_issue_age_s(state)
    median = stats.issue_median_s(state)
    done = len(stats.closed_at(state))
    total = stats.total_issues(state)
    return {
        "st": _status(state),
        "cur": _fit(label, profile),
        "stp": _fit(step, profile),
        "el": _hhmm(state.get("elapsed_s")),
        "ct": _mins(age),
        "cc": stats.issue_age_class(age, median),
        "pct": _pct(done, total),
        "idit": f"{done}/{total}",
        "eta": _eta(state),
    }


def _velocity(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    buckets = stats.velocity_buckets(state)
    total = stats.total_issues(state)
    return {
        "sp": stats.spark(buckets),
        "now": f"{stats.issues_per_hour(state):.0f}/h",
        "med": _mmss(stats.issue_median_s(state)),
        "left": max(0, total - len(stats.closed_at(state))),
    }


def _plan(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    flags = stats.version_flags(state)
    return {
        "vs": flags,
        "done": f"{flags.count(stats.FLAG_DONE)}/{len(flags)}",
    }


def _friction(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    metrics = state.get("metrics") or {}
    retried = stats.retried_issues(state)
    total = stats.total_issues(state)
    first_pass = float(metrics.get("first_pass_rate") or 0.0)
    return {
        "fp": f"{first_pass * 100:.0f}%",
        "rt": f"{len(retried)}/{total}",
        "pct": _pct(len(retried), total),
        # Two entries, so the frame is bounded by construction rather than by luck.
        "top": [_fit(f"{name} x{count}", profile) for name, count in retried[:2]],
        "fnd": stats.findings_by_severity(state),
        "open": _fit(stats.open_finding(state), profile),
    }


def _analytics(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    series = stats.tests_series(state)
    metrics = state.get("metrics") or {}
    return {
        "st": stats.step_table(state),
        "tp": str(int(metrics.get("tests_passing") or 0)),
        "tps": stats.spark(_thin(series, 8)),
        "cov": stats.coverage_pct(state),
    }


def _burndown(state: Mapping[str, Any], profile: Profile) -> dict[str, Any]:
    eta = state.get("eta") or {}
    return {
        "bd": "".join(f"{min(v, 99):02d}" for v in stats.burndown(state)),
        "tot": stats.total_issues(state),
        "est": stats.estimated_issues(state),
        "rem": max(0, stats.total_issues(state) - len(stats.closed_at(state))),
        "el": int(float(state.get("elapsed_s") or 0.0)),
        "lo": int(eta.get("low_s") or 0),
        "hi": int(eta.get("high_s") or 0),
        "err": stats.estimate_error_pct(state),
    }


_BUILDERS = {
    devices.SCREEN_NOW: _now,
    devices.SCREEN_VELOCITY: _velocity,
    devices.SCREEN_PLAN: _plan,
    devices.SCREEN_FRICTION: _friction,
    devices.SCREEN_ANALYTICS: _analytics,
    devices.SCREEN_BURNDOWN: _burndown,
}


# ── formatting ───────────────────────────────────────────────────────────────
#
# All of it here rather than on the device: a panel that formatted its own numbers
# would need locale rules, rounding rules and a clock, and every one of those is a
# thing to get wrong in C++ and verify by eye.


def _status(state: Mapping[str, Any]) -> str:
    """Four characters at most — the header has a dot beside it doing the real work."""
    return {"running": "run", "done": "done", "aborted": "abrt"}.get(
        str(state.get("status")), str(state.get("status"))[:4]
    )


def _hhmm(seconds: Any) -> str:
    """``HH:MM``. Minutes, not seconds: at a 15 s poll a seconds display would visibly
    jump, and a clock that lies about its own precision is worse than a coarser one."""
    total = int(float(seconds or 0.0))
    return f"{total // 3600:02d}:{total % 3600 // 60:02d}"


def _mmss(seconds: float | None) -> str:
    if seconds is None:
        return "-"
    total = int(seconds)
    return f"{total // 60}:{total % 60:02d}"


def _mins(seconds: float | None) -> str:
    """Coarse on purpose — this is a glanceable pill, not a stopwatch."""
    if seconds is None:
        return "-"
    minutes = int(seconds) // 60
    return f"{minutes}m" if minutes else f"{int(seconds)}s"


def _pct(done: int, total: int) -> int:
    return int(round(done / total * 100)) if total > 0 else 0


def _eta(state: Mapping[str, Any]) -> str:
    """A range, never a point, and blank until there is something to base it on.

    A point estimate from zero samples is a guess wearing a number's clothing, and it
    is worse on a device trusted at a glance.
    """
    eta = state.get("eta")
    if not isinstance(eta, Mapping):
        return "-"
    low, high = int(eta.get("low_s") or 0), int(eta.get("high_s") or 0)
    if high <= 0:
        return "-"
    return f"{low // 60}-{high // 60} min"


def _fit(text: str, profile: Profile) -> str:
    """Truncate to what the board can show.

    This is why a frame cannot outgrow the screen it feeds: the bound is the display,
    applied here, rather than a byte budget applied hopefully.
    """
    limit = profile.chars_per_line
    return text if len(text) <= limit else text[: limit - 1] + "."


def _thin(values: list[int], count: int) -> list[int]:
    """Even sample down to ``count`` points, keeping the last."""
    if len(values) <= count:
        return values
    step = (len(values) - 1) / (count - 1)
    return [values[min(int(round(i * step)), len(values) - 1)] for i in range(count)]

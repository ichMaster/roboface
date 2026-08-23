"""Derived statistics — every number and every graph the panels show.

All of it lives here rather than in C++, which is the point of vision §3.1: the device
renders JSON and computes nothing. Moving the arithmetic to the bridge moves it out of
the only place that can be checked by eye and into the place ``pytest`` already reaches.

Three of these read around defects in the tracking system's own instrumentation
(vision §9), and do so deliberately rather than waiting on a fix:

* **ANALYTICS** takes per-step-type *medians over closed spans only*. A sum is
  distorted by a single unclosed node; a median is not.
* **BURNDOWN** reads ``issue.closed`` from the tree rather than ``state.burndown``,
  which tracks decomposition and is non-monotonic.
* **NOW** builds its label from the deepest *running* node rather than
  ``state.current``, which reduces to the step name repeated three times.

Pure throughout: every function takes state and returns numbers. No clock, no I/O.
"""

from __future__ import annotations

import statistics
from collections.abc import Iterator, Mapping, Sequence
from typing import Any

#: Fixed order for the ANALYTICS table. The frame ships no names — the firmware knows
#: the labels, so only ``[n, median_s, share]`` travels.
STEP_ORDER = (
    "execute-issues",
    "upload-issues",
    "review-and-fix-issues",
    "release-version",
    "generate-issues",
)

#: Velocity buckets, in seconds. Matches the 30-minute granularity vision §2.2 measured.
BUCKET_S = 1800

#: Sparkline levels. One digit per bucket, so the frame carries ``"7511232"`` and the
#: firmware draws eight rectangle heights — never a glyph.
SPARK_MAX = 7

#: Colour classes for the issue-age pill: ok, warn, alarm.
CC_OK, CC_WARN, CC_ALARM = 0, 1, 2


def walk(nodes: Sequence[Mapping[str, Any]]) -> Iterator[Mapping[str, Any]]:
    """Every node in the tree, depth first."""
    for node in nodes:
        yield node
        children = node.get("children")
        if children:
            yield from walk(children)


def _of_kind(state: Mapping[str, Any], kind: str) -> list[Mapping[str, Any]]:
    return [n for n in walk(state.get("tree") or []) if n.get("kind") == kind]


# ── NOW ──────────────────────────────────────────────────────────────────────


def running_label(state: Mapping[str, Any]) -> tuple[str, str]:
    """``(version issue, step)`` for the deepest running node.

    **Not read from ``state.current``**, which in the measured run reduces to
    ``"execute-issues · execute-issues · execute-issues"`` — the step name three times,
    with no version and no issue (vision §9.4). The tree still has the truth; only the
    derived field is degenerate.

    A finished run has no running node, so this returns the last *completed* position
    instead of nothing: a panel showing a finished run should say where it ended.
    """
    running: dict[str, str] = {}
    latest: dict[str, str] = {}
    for node in walk(state.get("tree") or []):
        kind = str(node.get("kind"))
        if kind not in {"version", "step", "issue"}:
            continue
        name = str(node.get("id") or "")
        if node.get("status") == "running":
            running[kind] = name
        elif node.get("status") == "ok":
            # Later overwrites earlier, so a finished run reports where it *ended*.
            latest[kind] = name

    # Per kind, not all-or-nothing. The measured run has 8 step nodes still marked
    # running while every version has finished, so an all-or-nothing fallback picks up
    # those orphan steps and reports no version and no issue at all.
    picked = {kind: running.get(kind) or latest.get(kind, "") for kind in
              ("version", "step", "issue")}
    head = " ".join(p for p in (picked["version"], picked["issue"]) if p)
    return head, picked["step"]


def current_issue_age_s(state: Mapping[str, Any]) -> float | None:
    """Elapsed on the running issue, or ``None`` when nothing is running."""
    for node in walk(state.get("tree") or []):
        if node.get("kind") == "issue" and node.get("status") == "running":
            return float(node.get("elapsed_s") or 0.0)
    return None


def issue_age_class(age_s: float | None, median_s: float | None) -> int:
    """How alarming the current issue's age is, against the median for its kind.

    Issue duration has a 13x spread in the measured run — median 1.7 min, max 23.2 —
    so "running 15 minutes" is a signal that exists nowhere else. Thresholds are
    multiples of the median rather than absolute seconds, so a faster or slower
    project needs no retuning.
    """
    if age_s is None or not median_s:
        return CC_OK
    if age_s >= median_s * 5:
        return CC_ALARM
    if age_s >= median_s * 3:
        return CC_WARN
    return CC_OK


def issue_median_s(state: Mapping[str, Any]) -> float | None:
    """Median duration of completed issues.

    The median, not ``metrics.mean_issue_s``: that reports 631 s against a real median
    of 102 s, because the mean is dragged by a long tail.
    """
    done = [
        float(n.get("elapsed_s") or 0.0)
        for n in _of_kind(state, "issue")
        if n.get("end")
    ]
    return statistics.median(done) if done else None


# ── VELOCITY ─────────────────────────────────────────────────────────────────


def closed_at(state: Mapping[str, Any]) -> list[float]:
    """Seconds-from-start at which each issue closed, ascending."""
    started = _parse(state.get("started"))
    if started is None:
        return []
    out = [
        _parse(n.get("end")) - started  # type: ignore[operator]
        for n in _of_kind(state, "issue")
        if n.get("end") and _parse(n.get("end")) is not None
    ]
    return sorted(float(v) for v in out)


def velocity_buckets(state: Mapping[str, Any]) -> list[int]:
    """Issues closed per 30-minute bucket.

    The measured run reads ``15 7 2 2 6 4 6`` — a real stall between 1.5 and 2 hours
    that no other view surfaces.
    """
    closes = closed_at(state)
    span = float(state.get("elapsed_s") or 0.0)
    if not closes or span <= 0:
        return []
    count = max(1, int(span // BUCKET_S) + (1 if span % BUCKET_S else 0))
    buckets = [0] * count
    for moment in closes:
        index = min(int(moment // BUCKET_S), count - 1)
        buckets[index] += 1

    # Drop a trailing bucket covering less than half its width. A five-minute bucket
    # holding zero closures is not "velocity fell to zero" — it is a bucket that has
    # barely started, and drawing it puts a cliff on the sparkline that never happened.
    # A *leading* zero is kept: the opening half-hour genuinely closes nothing, because
    # generate and upload produce no closures, and that shelf is real.
    if len(buckets) > 1 and span % BUCKET_S and span % BUCKET_S < BUCKET_S / 2:
        buckets.pop()
    return buckets


def spark(values: Sequence[int]) -> str:
    """Quantise to one digit per value, 0-7. The firmware draws rectangles from these."""
    if not values:
        return ""
    peak = max(values)
    if peak <= 0:
        return "0" * len(values)
    return "".join(str(round(v / peak * SPARK_MAX)) for v in values)


def issues_per_hour(state: Mapping[str, Any]) -> float:
    span = float(state.get("elapsed_s") or 0.0)
    done = len(closed_at(state))
    return (done / (span / 3600)) if span > 0 else 0.0


# ── PLAN ─────────────────────────────────────────────────────────────────────

#: One ASCII flag per version. Never a glyph — the firmware draws circles from these.
FLAG_DONE, FLAG_RUNNING, FLAG_PENDING, FLAG_FAILED = "#", ">", ".", "!"


def version_flags(state: Mapping[str, Any]) -> str:
    """One character per planned version, in plan order."""
    by_id = {str(n.get("id")): n for n in _of_kind(state, "version")}
    flags = []
    for version in state.get("plan") or []:
        node = by_id.get(str(version))
        status = str(node.get("status")) if node else ""
        if status == "ok":
            flags.append(FLAG_DONE)
        elif status == "running":
            flags.append(FLAG_RUNNING)
        elif status in {"fail", "aborted"}:
            flags.append(FLAG_FAILED)
        else:
            flags.append(FLAG_PENDING)
    return "".join(flags)


# ── FRICTION ─────────────────────────────────────────────────────────────────


def retried_issues(state: Mapping[str, Any]) -> list[tuple[str, int]]:
    """``(issue id, attempts)`` for issues that needed more than one, worst first."""
    out = []
    for node in _of_kind(state, "issue"):
        attempts = int((node.get("data") or {}).get("attempts") or 1)
        if attempts > 1:
            out.append((str(node.get("id")), attempts))
    return sorted(out, key=lambda pair: (-pair[1], pair[0]))


def findings_by_severity(state: Mapping[str, Any]) -> list[int]:
    """``[high, medium, low]`` over every finding raised."""
    counts = {"HIGH": 0, "MEDIUM": 0, "LOW": 0}
    for finding in state.get("findings") or []:
        severity = str(finding.get("severity", "")).upper()
        if severity in counts:
            counts[severity] += 1
    return [counts["HIGH"], counts["MEDIUM"], counts["LOW"]]


def open_finding(state: Mapping[str, Any]) -> str:
    """The oldest unfixed finding, as ``version SEVERITY``. Empty when none."""
    for finding in state.get("findings") or []:
        if finding.get("outcome") not in {"fixed"}:
            return f"{finding.get('version', '')} {finding.get('severity', '')}".strip()
    return ""


# ── ANALYTICS ────────────────────────────────────────────────────────────────


def step_spans(state: Mapping[str, Any]) -> dict[str, list[float]]:
    """Durations per step type, **closed spans only**.

    Excluding unclosed spans is what makes this survivable on damaged data: a node
    that never closed carries no honest duration, and including it would let one
    orphan dominate the table (vision §9.1).
    """
    spans: dict[str, list[float]] = {name: [] for name in STEP_ORDER}
    for node in _of_kind(state, "step"):
        name = str(node.get("id"))
        if name in spans and node.get("end"):
            spans[name].append(float(node.get("elapsed_s") or 0.0))
    return spans


def step_table(state: Mapping[str, Any]) -> list[list[int]]:
    """``[count, median_s, share_pct]`` per step type, in :data:`STEP_ORDER`.

    Medians rather than sums, so an unclosed 155-minute node cannot distort it. The
    share is of total closed-span time, which is what the ``cov`` badge qualifies.
    """
    spans = step_spans(state)
    total = sum(sum(v) for v in spans.values()) or 1.0
    rows = []
    for name in STEP_ORDER:
        values = spans[name]
        median = int(statistics.median(values)) if values else 0
        share = int(round(sum(values) / total * 100)) if values else 0
        rows.append([len(values), median, share])
    return rows


def coverage_pct(state: Mapping[str, Any]) -> int:
    """Share of the run covered by closed step spans.

    **Mandatory on screen.** The table is built from a fraction of the run — 42% in
    the measured one — and a screen that omitted this would look complete when it is
    not. It measures emission completeness: it rises only when the skills stop
    dropping ``step.end``, which the reducer fix for #113 deliberately did not change.
    """
    span = float(state.get("elapsed_s") or 0.0)
    if span <= 0:
        return 0
    covered = sum(sum(v) for v in step_spans(state).values())
    return max(0, min(100, int(round(covered / span * 100))))


def tests_series(state: Mapping[str, Any]) -> list[int]:
    """Tests passing over time, one sample per issue that reported a count."""
    out = []
    for node in _of_kind(state, "issue"):
        passed = ((node.get("data") or {}).get("pytest") or {}).get("passed")
        if isinstance(passed, int):
            out.append(passed)
    return out


# ── BURNDOWN ─────────────────────────────────────────────────────────────────

#: Samples in the remaining-issues series. Two digits each, so 20 costs 40 characters.
BURNDOWN_POINTS = 20


def burndown(state: Mapping[str, Any], points: int = BURNDOWN_POINTS) -> list[int]:
    """Issues remaining, sampled evenly across the run.

    Built from issue closures rather than ``state.burndown``, whose ``known_points``
    oscillates ``0 -> 7 -> 0 -> 5 -> 0`` because it tracks decomposition rather than
    completion (vision §9.3). This series is monotonically non-increasing by
    construction, which the other cannot be.
    """
    span = float(state.get("elapsed_s") or 0.0)
    total = total_issues(state)
    if span <= 0 or points < 2:
        return []
    closes = closed_at(state)
    series = []
    for index in range(points):
        moment = span * index / (points - 1)
        done = sum(1 for c in closes if c <= moment)
        series.append(max(0, total - done))
    return series


def total_issues(state: Mapping[str, Any]) -> int:
    """Known scope. ``scope.known`` once anything is decomposed, else what exists."""
    scope = state.get("scope") or {}
    known = scope.get("known")
    if isinstance(known, int) and known > 0:
        return known
    return len(_of_kind(state, "issue"))


def estimated_issues(state: Mapping[str, Any]) -> int:
    """What the run's own estimate predicted, for BURNDOWN's dashed reference line.

    Labelled *estimate* on screen and never *ideal*: the measured run predicted 58 and
    delivered 42, so a line implying you should be on it would be a daily lie. The gap
    is the measurement (vision §2.4).
    """
    accuracy = (state.get("estimate") or {}).get("accuracy") or []
    total = sum(float(row.get("estimated_mid") or 0) for row in accuracy)
    return int(round(total))


def estimate_error_pct(state: Mapping[str, Any]) -> int:
    """Signed percentage by which the estimate overshot actual scope."""
    actual = total_issues(state)
    estimated = estimated_issues(state)
    if actual <= 0 or estimated <= 0:
        return 0
    return int(round((estimated - actual) / actual * 100))


# ── shared ───────────────────────────────────────────────────────────────────


def _parse(value: Any) -> float | None:
    """ISO-8601 to epoch seconds. Local to this module; the tracker's own parser
    returns datetimes, and arithmetic on floats keeps the callers above readable."""
    if not isinstance(value, str) or not value:
        return None
    from datetime import datetime

    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None

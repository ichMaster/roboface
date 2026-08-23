"""Fold an event log into the dashboard's state. Pure.

``reduce(lines, now)`` reads nothing and writes nothing: ``now`` is a **parameter**,
never ``datetime.now()``. That is what makes golden fixtures possible (architecture
§6) — a reducer that read the clock could not produce byte-identical output twice.

Tolerance is part of the contract (architecture §5.3). A torn final line is counted
and skipped; a malformed or unknown-type line is quarantined with its line number.
Nothing is discarded silently: an observability system that loses data quietly is
lying.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

from tracker import schema

TS_FORMAT = "%Y-%m-%dT%H:%M:%S.%f%z"

#: One event, or one of its sub-objects. Aliased to keep handler signatures readable.
Evt = dict[str, Any]

#: Issue-size weights. The burn-down is size-weighted, so three S issues must not
#: outrank one L (vision §6.2).
SIZE_POINTS = {"S": 1, "M": 3, "L": 5}

#: Roadmap band for a version that has not been decomposed yet (architecture §3.2).
ISSUES_LOW, ISSUES_HIGH = 3, 7

#: Suffixes that close a node opened by ``*.start``.
CLOSERS = {"end", "skipped", "aborted"}


def parse_ts(value: str) -> datetime | None:
    try:
        return datetime.strptime(value, TS_FORMAT)
    except (ValueError, TypeError):
        return None


@dataclass
class Node:
    """One node of the run tree: run, phase, version, step or issue."""

    id: str
    kind: str
    status: str = "running"
    start: str | None = None
    end: str | None = None
    elapsed_s: float = 0.0
    data: dict[str, Any] = field(default_factory=dict)
    children: list[Node] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "id": self.id,
            "kind": self.kind,
            "status": self.status,
            "start": self.start,
            "end": self.end,
            "elapsed_s": round(self.elapsed_s, 3),
        }
        if self.data:
            out["data"] = self.data
        if self.children:
            out["children"] = [child.as_dict() for child in self.children]
        return out


@dataclass
class State:
    """The reduced view. Serialised verbatim to ``state.json``."""

    run_id: str = ""
    schema: int = schema.SCHEMA_VERSION
    status: str = "unknown"
    command: str = ""
    started: str | None = None
    ended: str | None = None
    elapsed_s: float = 0.0
    idle_s: float = 0.0
    current: str | None = None
    plan: list[str] = field(default_factory=list)
    tree: list[dict[str, Any]] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)
    scope: dict[str, Any] = field(default_factory=dict)
    github: dict[str, Any] = field(default_factory=dict)
    estimate: dict[str, Any] | None = None
    eta: dict[str, Any] | None = None
    findings: list[dict[str, Any]] = field(default_factory=list)
    burndown: list[dict[str, Any]] = field(default_factory=list)
    quarantine: list[dict[str, Any]] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {
            "run_id": self.run_id,
            "schema": self.schema,
            "status": self.status,
            "command": self.command,
            "started": self.started,
            "ended": self.ended,
            "elapsed_s": round(self.elapsed_s, 3),
            "idle_s": round(self.idle_s, 3),
            "current": self.current,
            "plan": self.plan,
            "tree": self.tree,
            "metrics": self.metrics,
            "scope": self.scope,
            "github": self.github,
            "estimate": self.estimate,
            "eta": self.eta,
            "findings": self.findings,
            "burndown": self.burndown,
            "quarantine": self.quarantine,
            "counts": self.counts,
        }


def reduce(lines: Any, now: datetime) -> State:  # noqa: A001 - the domain name
    """Fold log lines into a :class:`State`. Pure: ``now`` is injected, never read."""
    events, counts, quarantine = _parse(lines)
    state = State(counts=counts, quarantine=quarantine)
    if not events:
        state.counts.setdefault("events", 0)
        return state

    tracker = _Accumulator(now)
    for seq, event in enumerate(events):
        tracker.apply(seq, event)
    tracker.finalise(state)
    return state


def _parse(lines: Any) -> tuple[list[dict[str, Any]], dict[str, int], list[dict[str, Any]]]:
    """Split lines into usable events, counts and quarantine entries."""
    events: list[dict[str, Any]] = []
    quarantine: list[dict[str, Any]] = []
    raw = [line for line in lines]
    torn = 0

    for number, line in enumerate(raw, start=1):
        text = line.rstrip("\n")
        if not text.strip():
            continue
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            # A cut final line is a killed process; anywhere else it is corruption.
            if number == len(raw):
                torn += 1
            else:
                quarantine.append({"line": number, "reason": "invalid JSON"})
            continue
        problems = schema.validate(parsed)
        if problems:
            quarantine.append({"line": number, "reason": "; ".join(problems[:3])})
            continue
        events.append(parsed)

    return events, {
        "events": len(events),
        "torn": torn,
        "malformed": len(quarantine),
    }, quarantine


class _Accumulator:
    """Mutable fold state. Split out so :func:`reduce` stays readable."""

    def __init__(self, now: datetime) -> None:
        self.now = now
        self.root = Node(id="run", kind="run")
        self.nodes: dict[tuple[str, ...], Node] = {(): self.root}
        self.run_data: dict[str, Any] = {}
        self.run_id: str = ""
        self.estimate: dict[str, Any] | None = None
        self.status = "running"
        self.started: str | None = None
        self.ended: str | None = None
        self.last_ts: str | None = None
        self.idle_s = 0.0
        self.decomposed: dict[str, list[dict[str, Any]]] = {}
        self.released: set[str] = set()
        self.skipped: set[str] = set()
        self.issue_attempts: dict[str, int] = {}
        self.issue_durations: list[float] = []
        self.tests_passing = 0
        self.gh_created = 0
        self.gh_closed = 0
        self.commits = 0
        self.findings: dict[str, dict[str, Any]] = {}
        self.version_overheads: list[float] = []
        # Burn-down is the one panel needing shape over TIME, not a final
        # snapshot. Sampled at every event that changes remaining work.
        self.burndown: list[dict[str, Any]] = []
        self.issue_points: dict[str, int] = {}
        self.total_points = 0
        self.done_points = 0
        self.plan: list[str] = []
        self.harden_durations: list[float] = []

    # ── event dispatch ───────────────────────────────────────────────────

    def apply(self, seq: int, event: dict[str, Any]) -> None:
        etype = str(event["type"])
        scope = event.get("scope") or {}
        data = event.get("data") or {}
        ts = str(event.get("ts", ""))
        self.last_ts = ts

        if self.started is None:
            self.started = ts

        handler = getattr(self, f"_on_{etype.replace('.', '_')}", None)
        if handler is not None:
            handler(event, scope, data, ts)

        self._track_tree(etype, scope, data, ts, event.get("status"))

    # ── tree ─────────────────────────────────────────────────────────────

    def _path(self, scope: dict[str, Any]) -> tuple[str, ...]:
        return tuple(
            str(scope[key]) for key in ("phase", "version", "step", "issue") if scope.get(key)
        )

    def _ensure(self, path: tuple[str, ...]) -> Node:
        if path in self.nodes:
            return self.nodes[path]
        kind = ("run", "phase", "version", "step", "issue")[len(path)]
        node = Node(id=path[-1] if path else "run", kind=kind)
        parent = self._ensure(path[:-1])
        parent.children.append(node)
        self.nodes[path] = node
        return node

    def _track_tree(
        self, etype: str, scope: dict[str, Any], data: dict[str, Any],
        ts: str, status: Any,
    ) -> None:
        family, _, tail = etype.rpartition(".")
        # Findings, hardening and releases are scoped to a version but are NOT nodes of
        # the tree. Letting them through re-opened a closed version: harden.start
        # carries the last version's scope, so the node went back to "running" after
        # its version.end had already closed it.
        if family in {"finding", "harden", "release"} or etype.startswith("harden."):
            return
        if family == "run" and tail != "start":
            return
        path = self._path(scope)

        # Only a lifecycle transition may *create* a node. Anything else updates one
        # that already exists, or is ignored.
        #
        # issue.uploaded is the case that forced this: upload-issues emits it scoped to
        # step=upload-issues, where no issue node was ever opened. Materializing one
        # left every uploaded issue permanently "running" under the upload step -- they
        # have no start and no end, so they never closed and their elapsed grew forever.
        # Being the deepest nodes in the tree, they then captured the header's "now"
        # line, which reported already-finished uploads instead of the real work.
        if tail != "start" and tail not in CLOSERS and path not in self.nodes:
            return

        node = self._ensure(path)

        if tail == "start":
            node.start = node.start or ts
            node.status = "running"
            # issue.start carries size and area; the panels need them on the node, and
            # only the *.end branch was persisting data.
            node.data.update({k: v for k, v in data.items() if k in {"size", "area"}})
        elif tail in CLOSERS:
            node.end = ts
            node.status = {"end": str(status or "ok"), "skipped": "skip",
                           "aborted": "fail"}.get(tail, "ok")
            if data:
                keep = {"tag", "reason", "attempts"}
            node.data.update({k: v for k, v in data.items() if k in keep})

    # ── handlers ─────────────────────────────────────────────────────────

    def _on_run_start(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.run_data = d
        # run_id lives in the envelope, never in data -- reading it from `d` yielded ""
        # for every run. The dashboard happened to mask that by overwriting the field
        # after reducing, so only the other consumers saw the blank.
        self.run_id = str(e.get("run_id") or "")
        self.started = ts
        self.plan = list(d.get("plan") or [])
        self._sample(ts)

    def _on_run_estimate(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.estimate = d

    def _on_run_resumed(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.idle_s += float(d.get("gap_s", 0) or 0)
        # Reopen the run. Resuming exists precisely to continue a run that stopped
        # without closing, so a run that has been resumed is running again -- leaving
        # the terminal status set would show the rest of the work happening inside a
        # run the panel still calls finished.
        self.status = "running"
        self.ended = None

    def _on_run_end(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.status = "done"
        self.ended = ts

    def _on_run_aborted(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.status = "aborted"
        self.ended = ts

    def _on_version_decomposed(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        issues = list(d.get("issues") or [])
        self.decomposed[str(s.get("version"))] = issues
        for issue in issues:
            points = SIZE_POINTS.get(str(issue.get("size")), 3)
            self.issue_points[str(issue.get("id"))] = points
            self.total_points += points
        self._sample(ts)   # scope just grew: the step UP the vision doc describes

    def _on_version_end(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.released.add(str(s.get("version")))
        # Freeze the suite size as this version left it. `tests_passing` is a single
        # running figure -- correct for "how big is the suite now", useless for a
        # trajectory, because every version would read the same final number. The
        # dashboard's suite panel did exactly that and drew a dead-flat line that looked
        # like a measurement. Nothing new has to be emitted: the count is already in the
        # log, it just has to be stamped where a per-version reader can find it.
        node = self.nodes.get(self._path(s))
        if node is not None:
            node.data["tests_passing"] = self.tests_passing

    def _on_version_skipped(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.skipped.add(str(s.get("version")))

    def _on_issue_validate_end(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        issue = str(s.get("issue"))
        attempt = int(d.get("attempt", 1) or 1)
        self.issue_attempts[issue] = max(self.issue_attempts.get(issue, 0), attempt)
        self._record_tests(d)

    def _record_tests(self, d: Evt) -> None:
        """Adopt the newest reported suite size, whatever it is.

        Deliberately *latest*, not the running maximum. The panel answers "how big is
        the suite now"; a high-water mark would keep reporting a count the repo no
        longer has the moment tests are consolidated or removed, and would never
        recover. Every emitter is expected to report a full-suite run.
        """
        passed = (d.get("pytest") or {}).get("passed")
        if isinstance(passed, int) and not isinstance(passed, bool):
            self.tests_passing = passed

    def _on_issue_end(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.done_points += self.issue_points.get(str(s.get("issue")), 3)
        self._sample(ts)

    def _on_issue_uploaded(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.gh_created += 1

    def _on_issue_closed(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.gh_closed += 1

    def _on_issue_commit(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.commits += 1

    def _on_finding_fixed(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.commits += 1
        # A review fix changes the suite too. Without this the count stayed frozen at
        # whatever the last issue reported, so the panel disagreed with the repo from
        # the first fix onward.
        self._record_tests(d)
        self._finding(s, d, outcome="fixed")

    def _on_harden_finding_fixed(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self.commits += 1
        self._record_tests(d)
        self._finding(s, d, outcome="hardened")

    def _on_harden_finding_held(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self._finding(s, d, outcome="held")

    def _on_finding_raised(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self._finding(s, d, severity=str(d.get("severity", "")), outcome="open")

    def _on_finding_classified(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self._finding(s, d, disposition=str(d.get("disposition", "")))

    def _on_finding_deferred(self, e: Evt, s: Evt, d: Evt, ts: str) -> None:
        self._finding(s, d, outcome="deferred")

    def _finding(self, scope: Evt, data: Evt, **fields: Any) -> None:
        fid = str(data.get("finding", ""))
        if not fid:
            return
        entry = self.findings.setdefault(
            fid, {"id": fid, "version": scope.get("version"), "severity": "", "outcome": "open"}
        )
        entry.update({k: v for k, v in fields.items() if v})

    def _sample(self, ts: str) -> None:
        """Record remaining work at this instant.

        ``undecomposed`` is what makes the uncertainty band possible: at t=0 nothing is
        decomposed and the whole total is inference, which is exactly what the band has
        to show (vision §6.2).
        """
        start = parse_ts(self.started or "")
        moment = parse_ts(ts)
        if not start or not moment:
            return
        undecomposed = [v for v in self.plan if v not in self.decomposed and v not in self.skipped]
        self.burndown.append({
            "elapsed_s": max(0.0, (moment - start).total_seconds() - self.idle_s),
            "known_points": max(0, self.total_points - self.done_points),
            "undecomposed": len(undecomposed),
        })

    # ── finalise ─────────────────────────────────────────────────────────

    def finalise(self, state: State) -> None:
        run_end = parse_ts(self.ended or "") if self.ended else self.now
        self._close_elapsed(self.root, run_end)
        self.issue_durations = [
            node.elapsed_s
            for path, node in self.nodes.items()
            if node.kind == "issue" and node.end
        ]

        state.run_id = self.run_id
        state.command = str(self.run_data.get("command", ""))
        state.plan = list(self.run_data.get("plan") or [])
        state.started = self.started
        state.ended = self.ended
        state.status = self.status if self.status != "running" else "running"
        state.idle_s = self.idle_s

        start_dt = parse_ts(self.started or "")
        end_dt = parse_ts(self.ended or "") if self.ended else self.now
        if start_dt and end_dt:
            state.elapsed_s = max(0.0, (end_dt - start_dt).total_seconds() - self.idle_s)

        state.tree = [child.as_dict() for child in self.root.children]
        # The deepest still-running node, for the header's "now" line. Derived here so
        # the UI never has to walk the tree to answer "what is happening right now".
        deepest = [
            f"{node.id}" for path, node in sorted(self.nodes.items(), key=lambda kv: len(kv[0]))
            if path and node.status == "running"
        ]
        state.current = " · ".join(deepest[-3:]) if deepest else None
        state.findings = sorted(self.findings.values(), key=lambda f: str(f["id"]))
        state.burndown = self.burndown
        self._scope_and_eta(state)
        self._metrics(state)
        self._github(state)

    def _close_elapsed(self, node: Node, ceiling: datetime | None = None) -> None:
        """Elapsed per node, bounded by the nearest enclosing end.

        An unclosed node must not accrue against wall-clock without limit. A step
        whose version has already ended cannot still be running, so its elapsed is
        capped at that version's end rather than at ``now`` -- otherwise a skill that
        forgets one ``step.end`` produces a node that grows for as long as the log
        sits on disk, and reducing a finished run reports a different number every
        time you look at it.

        Measured before the fix: one ``execute-issues`` node reported 620,225 s
        (172 h) inside a run that took 14,702 s (4.1 h), and step totals summed to
        174 h. See issue #113.
        """
        limit = ceiling if ceiling is not None else self.now
        start = parse_ts(node.start or "")
        end = parse_ts(node.end or "") if node.end else limit
        if start and end:
            node.elapsed_s = max(0.0, (end - start).total_seconds())
        # A closed node caps its children; an unclosed one passes its own cap down.
        child_ceiling = end if (node.end and end is not None) else limit
        for child in node.children:
            self._close_elapsed(child, child_ceiling)

    def _scope_and_eta(self, state: State) -> None:
        planned = [v for v in state.plan if v not in self.skipped]
        undecomposed = [v for v in planned if v not in self.decomposed]

        known_points = sum(
            SIZE_POINTS.get(str(issue.get("size")), 3)
            for issues in self.decomposed.values()
            for issue in issues
        )
        known_issues = sum(len(issues) for issues in self.decomposed.values())
        mean_issues = (known_issues / len(self.decomposed)) if self.decomposed else None
        low = int(mean_issues) if mean_issues else ISSUES_LOW
        high = int(mean_issues) if mean_issues else ISSUES_HIGH

        state.scope = {
            "known": known_issues,
            "known_points": known_points,
            "est_low": known_issues + len(undecomposed) * low,
            "est_high": known_issues + len(undecomposed) * high,
            "undecomposed": undecomposed,
        }

        if self.estimate:
            state.estimate = dict(self.estimate)
            state.estimate["accuracy"] = self._estimate_accuracy()

        done = sum(1 for issue in self.issue_attempts)
        # Recent velocity, not the whole run's: early issues on a fresh codebase run
        # slower (discovery, first-time setup) than later ones on an established
        # pattern, so an all-time mean drags the ETA toward a pessimistic past that
        # is no longer representative. Three is also the floor for showing an ETA at
        # all -- one or two samples is too noisy to be worth showing (vision §6.2:
        # an ETA computed from too little is worse than no ETA).
        recent_durations = self.issue_durations[-3:]
        if len(recent_durations) >= 3:
            mean_issue = sum(recent_durations) / len(recent_durations)
            remaining_low = max(0, state.scope["est_low"] - done)
            remaining_high = max(0, state.scope["est_high"] - done)
            state.eta = {
                "low_s": int(remaining_low * mean_issue),
                "high_s": int(remaining_high * mean_issue),
                "basis": {
                    "issues_sampled": len(recent_durations),
                    "undecomposed_versions": len(undecomposed),
                },
            }

    def _estimate_accuracy(self) -> list[dict[str, Any]]:
        """Signed error per version: actual minus estimated. Never used to correct."""
        if not self.estimate:
            return []
        rows: list[dict[str, Any]] = []
        for entry in self.estimate.get("versions") or []:
            version = str(entry.get("id"))
            if version not in self.decomposed:
                continue
            actual = len(self.decomposed[version])
            mid = (int(entry.get("issues_low", 0)) + int(entry.get("issues_high", 0))) / 2
            rows.append({"version": version, "estimated_mid": mid, "actual": actual,
                         "error": actual - mid})
        return rows

    def _metrics(self, state: State) -> None:
        attempts = list(self.issue_attempts.values())
        first_pass = sum(1 for a in attempts if a == 1)
        state.metrics = {
            "issues_done": len(attempts),
            "first_pass_rate": round(first_pass / len(attempts), 4) if attempts else None,
            "retried_issues": sum(1 for a in attempts if a > 1),
            "mean_issue_s": (
                round(sum(self.issue_durations) / len(self.issue_durations), 2)
                if self.issue_durations else None
            ),
            "tests_passing": self.tests_passing,
            "versions_released": len(self.released),
            "versions_skipped": len(self.skipped),
            "findings_open": sum(
                1 for f in self.findings.values() if f["outcome"] in {"open", "deferred"}
            ),
            "findings_total": len(self.findings),
        }

    def _github(self, state: State) -> None:
        git = self.run_data.get("git") or {}
        state.github = {
            "created": self.gh_created,
            "closed": self.gh_closed,
            "open": max(0, self.gh_created - self.gh_closed),
            "commits": self.commits,
            "branch": git.get("branch"),
            "head_sha": git.get("head_sha"),
        }

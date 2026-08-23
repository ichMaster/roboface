"""Synthetic log generator — realistic runs without running the pipeline.

TRK-024. Resolves a chicken-and-egg: fixtures must be realistic, but a real log only
exists once the skills are instrumented, and a forty-minute run with real commits and
GitHub issues is not an iteration loop.

**Deterministic.** A scenario plus a seed produces the same bytes every time, so the
output can be committed as a fixture and diffed. Timestamps advance by a fixed step
rather than reading the clock — the reducer is pure for the same reason (architecture
§6), and a generator that used ``datetime.now`` would make golden files impossible.

Test-only. Nothing under ``tracker/``, ``hooks/`` or ``dashboard/`` may import this.
"""

from __future__ import annotations

import json
import random
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from typing import Any

from tracker import emit, schema

EMITTER_SHIP = "skill:ship-phase"
EMITTER_EXEC = "skill:execute-issues"
EMITTER_REVIEW = "skill:review-and-fix-issues"
EMITTER_HARDEN = "skill:harden-findings"
EMITTER_RELEASE = "skill:release-version"

RUN_ID = "run-20260803-142012"
START = datetime(2026, 8, 3, 14, 20, 12, tzinfo=UTC)

SIZES = ["S", "M", "L"]
AREAS = ["games", "server", "agent", "web"]


@dataclass
class VersionSpec:
    """One version's shape in a scenario."""

    version: str
    phase: str
    issues: int = 4
    retries: dict[int, int] = field(default_factory=dict)  # issue index -> attempts
    findings: tuple[int, int, int] = (0, 0, 0)  # fix-now, deferred, held
    reviewed: bool = True
    released: bool = True
    skipped: bool = False


@dataclass
class Scenario:
    """A whole run, described compactly enough to read at a glance."""

    name: str
    command: str = "/ship-phase v01"
    versions: list[VersionSpec] = field(default_factory=list)
    seed: int = 1
    decompose: bool = True
    finish: bool = True
    abort_after: str | None = None
    harden: bool = True
    #: Tests each issue adds to the suite. Fixed at 6, ten versions topped out under
    #: 200 -- which is exactly where a chart axis had been hardcoded, so the fixture
    #: could never reach the value that broke it. A fixture that cannot exceed the
    #: constant it is meant to test is not testing it.
    tests_per_issue: int = 6
    estimate: bool = True


class _Writer:
    """Accumulates events with a monotonic synthetic clock."""

    def __init__(self, seed: int) -> None:
        self.lines: list[str] = []
        self.clock = START
        self.rng = random.Random(seed)
        self.issue_number = 0

    def tick(self, seconds: int) -> None:
        self.clock += timedelta(seconds=seconds)

    def add(
        self,
        event_type: str,
        emitter: str,
        scope: dict[str, str] | None = None,
        status: str | None = None,
        data: dict[str, Any] | None = None,
    ) -> None:
        event = emit.build(
            event_type,
            emitter=emitter,
            scope=scope,
            status=status,
            data=data,
            run_id=RUN_ID,
            ts=self.clock.strftime("%Y-%m-%dT%H:%M:%S.") + f"{self.clock.microsecond // 1000:03d}Z",
        )
        problems = schema.validate(event)
        if problems:  # a generator that emits invalid events is worse than none
            raise ValueError(f"{event_type}: {problems}")
        self.lines.append(json.dumps(event, separators=(",", ":"), ensure_ascii=False))

    def next_issue(self) -> str:
        self.issue_number += 1
        return f"RF-{self.issue_number:03d}"


def generate(scenario: Scenario) -> str:
    """Render a scenario to JSONL text."""
    w = _Writer(scenario.seed)
    plan = [v.version for v in scenario.versions]
    tests = 0

    w.add(
        "run.start", EMITTER_SHIP,
        data={
            "command": scenario.command,
            "plan": plan,
            "baseline": {"tests": 0, "mypy_errors": 0},
            "git": {"branch": "codegen-tracking", "head_sha": "abc1234", "remote": "origin"},
        },
    )
    if scenario.estimate:
        w.tick(3)
        w.add(
            "run.estimate", EMITTER_SHIP,
            data={
                "source": "estimated",
                "versions": [
                    {"id": v.version, "issues_low": 3, "issues_high": 7} for v in scenario.versions
                ],
                "total": {"issues_low": 3 * len(plan), "issues_high": 7 * len(plan)},
            },
        )

    for phase in sorted({v.phase for v in scenario.versions}):
        in_phase = [v for v in scenario.versions if v.phase == phase]
        w.tick(2)
        w.add("phase.start", EMITTER_SHIP, scope={"phase": phase})

        for spec in in_phase:
            tests = _emit_version(w, scenario, spec, tests)
            if scenario.abort_after and spec.version == scenario.abort_after:
                return "\n".join(w.lines) + "\n"

        if scenario.harden:
            _emit_harden(w, phase, in_phase)
        w.tick(2)
        w.add("phase.end", EMITTER_SHIP, status="ok", scope={"phase": phase},
              data={"versions": len(in_phase)})

    if scenario.finish:
        w.tick(2)
        w.add(
            "run.end", EMITTER_SHIP, status="ok",
            data={
                "versions_done": sum(1 for v in scenario.versions if v.released),
                "issues_done": w.issue_number,
            },
        )
    return "\n".join(w.lines) + "\n"


def _emit_version(w: _Writer, scenario: Scenario, spec: VersionSpec, tests: int) -> int:
    scope_v = {"phase": spec.phase, "version": spec.version}

    if spec.skipped:
        w.tick(1)
        w.add("version.skipped", EMITTER_SHIP, status="skip", scope=scope_v,
              data={"reason": "already-released"})
        return tests

    w.tick(2)
    w.add("version.start", EMITTER_SHIP, scope=scope_v)

    issues = [
        {"id": w.next_issue(), "size": SIZES[w.rng.randrange(len(SIZES))]}
        for _ in range(spec.issues)
    ]

    for step in ("generate-issues", "upload-issues"):
        scope_s = {**scope_v, "step": step}
        w.tick(5)
        w.add("step.start", EMITTER_SHIP, scope=scope_s)
        if step == "generate-issues" and scenario.decompose:
            w.tick(60)
            w.add("version.decomposed", EMITTER_SHIP, status="ok", scope=scope_v,
                  data={"issues": issues})
        w.tick(20)
        w.add("step.end", EMITTER_SHIP, status="ok", scope=scope_s)

    tests = _emit_execute(w, spec, scope_v, issues, tests, scenario.tests_per_issue)
    if spec.reviewed:
        _emit_review(w, spec, scope_v)
    if spec.released:
        _emit_release(w, spec, scope_v)
        w.tick(2)
        w.add("version.end", EMITTER_SHIP, status="ok", scope=scope_v,
              data={"tag": f"{spec.version}.00"})
    return tests


def _emit_execute(
    w: _Writer, spec: VersionSpec, scope_v: dict[str, str], issues: list[Any], tests: int,
    per_issue: int = 6,
) -> int:
    scope_s = {**scope_v, "step": "execute-issues"}
    w.tick(4)
    w.add("step.start", EMITTER_SHIP, scope=scope_s)

    for index, issue in enumerate(issues):
        scope_i = {**scope_s, "issue": issue["id"]}
        attempts = spec.retries.get(index, 1)
        w.tick(3)
        w.add("issue.start", EMITTER_EXEC, scope=scope_i,
              data={"size": issue["size"], "area": AREAS[w.rng.randrange(len(AREAS))]})
        w.tick(10)
        w.add("issue.uploaded", EMITTER_EXEC, status="ok", scope=scope_i,
              data={"issue": issue["id"], "gh_number": index + 1,
                    "url": f"https://example.invalid/{index + 1}"})

        for attempt in range(1, attempts + 1):
            w.tick(90)
            w.add("issue.implement.end", EMITTER_EXEC, status="ok", scope=scope_i,
                  data={"files_changed": 2})
            failing = attempt < attempts
            w.tick(8)
            w.add(
                "issue.validate.end", EMITTER_EXEC, status="fail" if failing else "ok",
                scope=scope_i,
                data={
                    "attempt": attempt,
                    "pytest": {"passed": tests, "failed": 2 if failing else 0, "duration_s": 5.5},
                    "mypy": {"errors": 0},
                },
            )
            if failing:
                w.add("issue.failed", EMITTER_EXEC, status="fail", scope=scope_i,
                      data={"attempt": attempt, "reason": "test-failure"})
                w.tick(1)
                w.add("issue.reverted", EMITTER_EXEC, status="fail", scope=scope_i,
                      data={"attempt": attempt})

        tests += per_issue
        w.tick(4)
        w.add("issue.commit", EMITTER_EXEC, status="ok", scope=scope_i,
              data={"sha": f"c{w.issue_number:06d}", "files": ["games/x.py"]})
        w.add("issue.closed", EMITTER_EXEC, status="ok", scope=scope_i,
              data={"issue": issue["id"], "gh_number": index + 1})
        w.add("issue.end", EMITTER_EXEC, status="ok", scope=scope_i, data={"attempts": attempts})

    w.tick(3)
    w.add("step.end", EMITTER_SHIP, status="ok", scope=scope_s)
    return tests


def _emit_review(w: _Writer, spec: VersionSpec, scope_v: dict[str, str]) -> None:
    scope_s = {**scope_v, "step": "review-and-fix-issues"}
    fix_now, deferred, held = spec.findings
    w.tick(5)
    w.add("step.start", EMITTER_SHIP, scope=scope_s)

    index = 0
    for kind, count in (("fix-now", fix_now), ("defer", deferred), ("defer", held)):
        for _ in range(count):
            index += 1
            fid = f"{spec.version}-F{index}"
            severity = "HIGH" if index == 1 else "MEDIUM"
            w.tick(20)
            w.add("finding.raised", EMITTER_REVIEW, scope=scope_v,
                  data={"finding": fid, "severity": severity, "title": "a real defect"})
            w.add("finding.classified", EMITTER_REVIEW, scope=scope_v,
                  data={"finding": fid, "disposition": kind})
            if kind == "fix-now":
                w.tick(120)
                w.add("finding.fixed", EMITTER_REVIEW, status="ok", scope=scope_v,
                      data={"finding": fid, "sha": f"f{index:06d}"})
            else:
                w.add("finding.deferred", EMITTER_REVIEW, status="skip", scope=scope_v,
                      data={"finding": fid, "home": "v05.01"})
    w.tick(4)
    w.add("step.end", EMITTER_SHIP, status="ok", scope=scope_s)


def _emit_release(w: _Writer, spec: VersionSpec, scope_v: dict[str, str]) -> None:
    scope_s = {**scope_v, "step": "release-version"}
    tag = f"{spec.version}.00"
    w.tick(3)
    w.add("step.start", EMITTER_SHIP, scope=scope_s)
    w.tick(12)
    w.add("release.tagged", EMITTER_RELEASE, status="ok", scope=scope_v, data={"tag": tag})
    w.add("release.pushed", EMITTER_RELEASE, status="ok", scope=scope_v,
          data={"tag": tag, "remote": "origin"})
    w.add("step.end", EMITTER_SHIP, status="ok", scope=scope_s)


def _emit_harden(w: _Writer, phase: str, specs: list[VersionSpec]) -> None:
    last = specs[-1]
    scope_v = {"phase": phase, "version": last.version}
    w.tick(4)
    w.add("harden.start", EMITTER_HARDEN, scope=scope_v)
    for spec in specs:
        _fix_now, deferred, held = spec.findings
        for i in range(deferred):
            w.tick(60)
            w.add("harden.finding.fixed", EMITTER_HARDEN, status="ok", scope=scope_v,
                  data={"finding": f"{spec.version}-F{i + 1}", "sha": f"h{i:06d}"})
        for i in range(held):
            w.tick(20)
            w.add("harden.finding.held", EMITTER_HARDEN, status="held", scope=scope_v,
                  data={"finding": f"{spec.version}-H{i + 1}", "reason": "needs a design decision"})


# ── the presets behind the golden fixtures (architecture §10.2) ──────────────

def _v(version: str, **kwargs: Any) -> VersionSpec:
    return VersionSpec(version=version, phase=version.split(".")[0], **kwargs)


PRESETS: dict[str, Scenario] = {
    "clean-run": Scenario(
        name="clean-run",
        versions=[_v("v01.01", issues=3, findings=(1, 0, 0)),
                  _v("v01.02", issues=4, findings=(1, 1, 0))],
    ),
    "retry-run": Scenario(
        name="retry-run", seed=7,
        versions=[_v("v01.01", issues=4, retries={1: 2, 3: 3}, findings=(1, 0, 0))],
    ),
    "aborted-run": Scenario(
        name="aborted-run", finish=False, harden=False, abort_after="v01.02",
        versions=[_v("v01.01", issues=2),
                  _v("v01.02", issues=3, reviewed=False, released=False)],
    ),
    "skipped-versions": Scenario(
        name="skipped-versions",
        versions=[_v("v01.01", skipped=True), _v("v01.02", issues=3, findings=(1, 0, 0))],
    ),
    "no-review": Scenario(
        name="no-review", harden=False,
        versions=[_v("v01.01", issues=3, findings=(2, 0, 0)),
                  _v("v01.02", issues=2, reviewed=False, released=False)],
        finish=False,
    ),
    "held-findings": Scenario(
        name="held-findings",
        versions=[_v("v01.01", issues=2, findings=(1, 1, 1))],
    ),
    # A run the size of the real validation workload: /ship-phase v03, ten versions
    # across three phases. Every other preset is one or two versions -- the same shape
    # as the dashboard prototype's mock data, which is precisely why a whole class of
    # layout faults survived. Panels that divided a fixed height by their row count gave
    # ten rows a NEGATIVE bar height, and axis maxima carried over from the mock were
    # exceeded, so a series left its own card and painted over the panel above it.
    # Nothing in a two-version fixture can catch either.
    "full-roadmap": Scenario(
        name="full-roadmap", command="/ship-phase v03", seed=3, tests_per_issue=14,
        versions=[
            _v("v01.01", issues=3, findings=(1, 0, 0)),
            _v("v01.02", issues=4, retries={2: 2}, findings=(1, 1, 0)),
            _v("v01.03", issues=3, findings=(2, 1, 0)),
            _v("v01.04", issues=5, retries={1: 3}, findings=(2, 2, 1)),
            _v("v02.01", issues=4, findings=(1, 2, 0)),
            _v("v02.02", issues=3, findings=(0, 2, 0)),
            _v("v02.03", issues=3, findings=(1, 1, 0)),
            _v("v03.01", issues=4, findings=(2, 2, 0)),
            _v("v03.02", issues=2, findings=(1, 1, 0)),
            _v("v03.03", issues=2, findings=(1, 2, 0)),
        ],
    ),
}


def preset(name: str) -> str:
    """Render a named preset."""
    return generate(PRESETS[name])


def torn_tail(name: str = "clean-run") -> str:
    """A preset whose final line is cut mid-write, as a killed process leaves it."""
    text = preset(name)
    return text[: -len(text.splitlines()[-1]) - 1] + '{"v":1,"ts":"2026-08-03T14:2'


def malformed(name: str = "clean-run") -> str:
    """A preset with a bad JSON line and an unknown event type spliced in."""
    lines = preset(name).splitlines()
    mid = len(lines) // 2
    unknown = json.dumps(
        {
            "v": 1, "ts": "2026-08-03T14:30:00.000Z", "run_id": RUN_ID,
            "type": "future.event", "emitter": "skill:ship-phase", "scope": {},
        },
        separators=(",", ":"),
    )
    return "\n".join(lines[:mid] + ["{not json at all", unknown] + lines[mid:]) + "\n"

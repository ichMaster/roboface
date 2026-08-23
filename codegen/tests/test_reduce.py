"""TRK-006 / TRK-007 / TRK-008 / TRK-009 — the reducer, its fixtures, and the writer.

Fixtures come from the generator (TRK-024) rather than being hand-authored, so adding
a failure mode means adding a scenario, not transcribing sixty lines of JSON.

The two tests that matter most are the purity check — asserted over the AST, because
a single ``datetime.now()`` would make every golden fixture unreproducible — and
``no-review``, which pins the distinction the prototype originally got wrong.
"""

from __future__ import annotations

import ast
import json
import subprocess
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest

from tests import gen_log
from tracker import emit, paths
from tracker import reduce as reduce_mod
from tracker import state as state_mod

NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)


def _reduce(name: str) -> reduce_mod.State:
    return reduce_mod.reduce(gen_log.preset(name).splitlines(), NOW)


# ── purity: what makes golden fixtures possible at all ───────────────────────


def test_reduce_reads_no_clock_no_env_and_no_files() -> None:
    """Asserted over the AST, not by inspection.

    ``reduce`` taking ``now`` as a parameter is the whole reason a fixture can be
    committed. One stray ``datetime.now()`` would break that silently, so the ban is
    mechanical.
    """
    source = Path(reduce_mod.__file__).read_text(encoding="utf-8")
    banned = {"now", "today", "time", "monotonic", "getenv", "open", "read_text"}
    offenders: list[str] = []
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and (
            node.func.attr in banned
        ):
            offenders.append(node.func.attr)
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and (
            node.func.id in {"open", "input"}
        ):
            offenders.append(node.func.id)
    assert not offenders, f"reduce.py must stay pure; found {sorted(set(offenders))}"


def test_reducing_twice_is_byte_identical() -> None:
    first = json.dumps(_reduce("clean-run").as_dict(), sort_keys=True)
    second = json.dumps(_reduce("clean-run").as_dict(), sort_keys=True)
    assert first == second


def test_now_only_affects_open_nodes() -> None:
    """A finished run's elapsed must not drift with the clock."""
    lines = gen_log.preset("clean-run").splitlines()
    early = reduce_mod.reduce(lines, NOW)
    late = reduce_mod.reduce(lines, NOW + timedelta(days=3))
    assert early.elapsed_s == late.elapsed_s
    assert early.status == late.status == "done"


# ── the golden fixtures, one per failure mode ────────────────────────────────


def test_clean_run_has_balanced_pairs_and_a_complete_tree() -> None:
    state = _reduce("clean-run")
    assert state.status == "done"
    assert state.counts["torn"] == 0 and state.counts["malformed"] == 0
    assert [node["id"] for node in state.tree] == ["v01"]
    versions = state.tree[0]["children"]
    assert [v["id"] for v in versions] == ["v01.01", "v01.02"]
    assert all(v["status"] == "ok" and v["end"] for v in versions)


def test_retry_run_lowers_the_first_pass_rate() -> None:
    state = _reduce("retry-run")
    assert state.metrics["retried_issues"] == 2
    assert state.metrics["first_pass_rate"] == 0.5


def test_aborted_run_reports_running_without_crashing() -> None:
    state = _reduce("aborted-run")
    assert state.status == "running"
    assert any(node["status"] == "running" for node in state.tree)


def test_torn_tail_is_counted_and_skipped() -> None:
    state = reduce_mod.reduce(gen_log.torn_tail().splitlines(), NOW)
    assert state.counts["torn"] == 1
    assert state.counts["events"] > 0, "everything before the tear must still reduce"


def test_malformed_lines_are_quarantined_with_line_numbers() -> None:
    state = reduce_mod.reduce(gen_log.malformed().splitlines(), NOW)
    reasons = " ".join(entry["reason"] for entry in state.quarantine)
    assert len(state.quarantine) == 2
    assert "invalid JSON" in reasons
    assert "unknown event type" in reasons
    assert all(isinstance(entry["line"], int) for entry in state.quarantine)
    assert state.counts["events"] > 0, "the rest of the file must still reduce"


def test_skipped_versions_are_excluded_from_scope() -> None:
    state = _reduce("skipped-versions")
    assert state.metrics["versions_skipped"] == 1
    assert "v01.01" not in state.scope["undecomposed"]


def test_a_version_without_a_review_is_absent_not_zero() -> None:
    """The prototype's bug, pinned.

    Zero findings and *not yet reviewed* are different claims. A version whose review
    step never ran must not appear among findings at all — rendering it as an empty
    bar asserts "clean" when it means "nobody looked".
    """
    state = _reduce("no-review")
    versions_with_findings = {f["version"] for f in state.findings}
    assert "v01.01" in versions_with_findings
    assert "v01.02" not in versions_with_findings


def test_held_findings_keep_their_outcome() -> None:
    state = _reduce("held-findings")
    outcomes = {f["outcome"] for f in state.findings}
    assert "held" in outcomes


# ── scope, estimate and ETA ──────────────────────────────────────────────────


def test_scope_is_a_range_and_never_a_planned_scalar() -> None:
    state = _reduce("clean-run")
    assert set(state.scope) >= {"known", "est_low", "est_high", "undecomposed"}
    assert "issues_planned" not in json.dumps(state.as_dict()), (
        "a single planned number would be a guess wearing the costume of a fact"
    )


def test_the_uncertainty_band_narrows_as_versions_are_decomposed() -> None:
    """Widest when nothing is known; zero once every version is decomposed."""
    lines = gen_log.preset("clean-run").splitlines()
    widths: list[int] = []
    for cut in (3, len(lines) // 2, len(lines)):
        state = reduce_mod.reduce(lines[:cut], NOW)
        widths.append(state.scope["est_high"] - state.scope["est_low"])
    assert widths[0] >= widths[-1]
    assert widths[-1] == 0, "no undecomposed versions should leave no uncertainty"


def test_estimate_accuracy_is_recorded_as_signed_error() -> None:
    state = _reduce("clean-run")
    assert state.estimate is not None
    rows = state.estimate["accuracy"]
    assert rows and all({"version", "estimated_mid", "actual", "error"} <= set(r) for r in rows)
    for row in rows:
        assert row["error"] == row["actual"] - row["estimated_mid"]


def test_eta_is_absent_until_three_issues_are_done() -> None:
    """Fewer than 3 samples is too noisy to show a number at all (not just a wider
    one) -- vision §6.2: an ETA computed from too little is worse than no ETA."""
    lines = gen_log.preset("clean-run").splitlines()
    assert reduce_mod.reduce(lines[:24], NOW).eta is None  # 2 issues closed so far
    assert reduce_mod.reduce(lines[:31], NOW).eta is not None  # the 3rd just closed


def test_eta_carries_its_own_basis() -> None:
    state = _reduce("clean-run")
    assert state.eta is not None
    assert state.eta["basis"]["issues_sampled"] > 0
    assert state.eta["low_s"] <= state.eta["high_s"]


def test_eta_uses_only_the_last_three_issue_durations() -> None:
    """Recent velocity, not the whole run's: an all-time mean drags the estimate
    toward early, slower issues (discovery, first-time setup) long after later ones
    have settled into a faster, more representative pace."""
    acc = reduce_mod._Accumulator(NOW)
    acc.plan = ["v01.01", "v01.02"]
    acc.decomposed = {"v01.01": [{"id": f"RF-{i:03d}", "size": "M"} for i in range(5)]}
    acc.issue_attempts = {f"RF-{i:03d}": 1 for i in range(5)}
    # Early issues were slow (600s); the three most recent were fast (60s).
    acc.issue_durations = [600.0, 600.0, 60.0, 60.0, 60.0]

    state = reduce_mod.State()
    acc._scope_and_eta(state)

    assert state.eta is not None
    assert state.eta["basis"]["issues_sampled"] == 3
    expected_mean = 60.0
    remaining = state.scope["est_low"] - len(acc.issue_attempts)
    assert state.eta["low_s"] == int(max(0, remaining) * expected_mean)


# ── github and idle ──────────────────────────────────────────────────────────


def test_github_counts_come_from_the_log() -> None:
    state = _reduce("clean-run")
    assert state.github["created"] == state.github["closed"] == 7
    assert state.github["open"] == 0
    assert state.github["commits"] >= 7
    assert state.github["branch"] == "codegen-tracking"


def test_elapsed_excludes_the_idle_gap(isolated_runs_dir: Path) -> None:
    """A run paused overnight must not report the pause as working time."""
    base = gen_log.preset("clean-run").splitlines()
    resumed = json.dumps(
        {
            "v": 1, "ts": "2026-08-03T15:00:00.000Z", "run_id": gen_log.RUN_ID,
            "type": "run.resumed", "emitter": "skill:ship-phase", "scope": {},
            "data": {"gap_s": 60},
        },
        separators=(",", ":"),
    )
    state = reduce_mod.reduce([*base[:-1], resumed, base[-1]], NOW)
    assert state.idle_s == 60
    plain = reduce_mod.reduce(base, NOW)
    assert state.elapsed_s == pytest.approx(plain.elapsed_s - 60, abs=1)


# ── the writer ───────────────────────────────────────────────────────────────


def test_state_is_written_atomically_and_reread(isolated_runs_dir: Path) -> None:
    run_id = gen_log.RUN_ID
    paths.events_path(run_id).parent.mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset("clean-run"), encoding="utf-8")

    state = state_mod.rebuild(run_id, NOW)
    assert state.run_id == run_id
    on_disk = state_mod.read(run_id)
    assert on_disk is not None
    assert on_disk["metrics"]["issues_done"] == state.metrics["issues_done"]


def test_deleting_state_loses_nothing(isolated_runs_dir: Path) -> None:
    """state.json is disposable; events.jsonl is the source of truth."""
    run_id = gen_log.RUN_ID
    paths.events_path(run_id).parent.mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset("clean-run"), encoding="utf-8")

    first = json.dumps(state_mod.rebuild(run_id, NOW).as_dict(), sort_keys=True)
    paths.state_path(run_id).unlink()
    second = json.dumps(state_mod.rebuild(run_id, NOW).as_dict(), sort_keys=True)
    assert first == second


def test_a_reader_never_sees_a_half_written_snapshot(isolated_runs_dir: Path) -> None:
    """Atomic replace: 200 rapid writes, read concurrently, never invalid JSON."""
    run_id = gen_log.RUN_ID
    paths.events_path(run_id).parent.mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset("clean-run"), encoding="utf-8")
    state = reduce_mod.reduce(gen_log.preset("clean-run").splitlines(), NOW)

    reader = subprocess.Popen(
        [
            sys.executable, "-c",
            "import json,sys,time\n"
            f"p = {str(paths.state_path(run_id))!r}\n"
            "bad = 0\n"
            "for _ in range(400):\n"
            "    try:\n"
            "        json.load(open(p))\n"
            "    except FileNotFoundError:\n"
            "        pass\n"
            "    except Exception:\n"
            "        bad += 1\n"
            "    time.sleep(0.001)\n"
            "sys.exit(bad)\n",
        ],
    )
    for _ in range(200):
        state_mod.write(run_id, state)
    assert reader.wait(timeout=60) == 0, "a reader observed a partial snapshot"


def test_rebuild_cli_reports_the_run(isolated_runs_dir: Path) -> None:
    run_id = gen_log.RUN_ID
    paths.events_path(run_id).parent.mkdir(parents=True, exist_ok=True)
    paths.events_path(run_id).write_text(gen_log.preset("clean-run"), encoding="utf-8")
    paths.current_pointer().write_text(run_id, encoding="utf-8")

    env = dict(__import__("os").environ, CODEGEN_RUNS_DIR=str(paths.runs_root()))
    result = subprocess.run(
        [sys.executable, "-m", "tracker.state"],
        cwd=paths.codegen_root(), env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stderr
    assert run_id in result.stdout and "events" in result.stdout


def test_emitted_events_reduce_end_to_end(isolated_runs_dir: Path) -> None:
    """The emitter and the reducer agree — not just the generator and the reducer."""
    run_id = "run-20260803-142012"
    paths.run_dir(run_id).mkdir(parents=True, exist_ok=True)
    paths.current_pointer().write_text(run_id, encoding="utf-8")

    emit.emit("run.start", emitter="skill:ship-phase", data={
        "command": "/ship-phase v01", "plan": ["v01.01"],
        "baseline": {"tests": 0, "mypy_errors": 0},
        "git": {"branch": "b", "head_sha": "s", "remote": "o"},
    })
    emit.emit("phase.start", emitter="skill:ship-phase", scope={"phase": "v01"})
    emit.emit("version.start", emitter="skill:ship-phase",
              scope={"phase": "v01", "version": "v01.01"})
    emit.emit("run.end", emitter="skill:ship-phase", status="ok",
              data={"versions_done": 0, "issues_done": 0})

    state = state_mod.rebuild(run_id, NOW)
    assert state.status == "done"
    assert state.command == "/ship-phase v01"
    assert state.counts["malformed"] == 0


# ── the burn-down series ─────────────────────────────────────────────────────


def test_burndown_is_a_time_series_not_a_final_snapshot() -> None:
    """The one panel needing shape over time; everything else reads current state."""
    state = _reduce("clean-run")
    assert len(state.burndown) >= 5
    assert all({"elapsed_s", "known_points", "undecomposed"} <= set(p) for p in state.burndown)
    assert [p["elapsed_s"] for p in state.burndown] == sorted(
        p["elapsed_s"] for p in state.burndown
    ), "samples must be chronological"


def test_remaining_work_steps_UP_when_a_version_is_decomposed() -> None:
    """Scope arriving is the signature the vision doc asks to be visible, not smoothed."""
    points = [p["known_points"] for p in _reduce("clean-run").burndown]
    assert any(b > a for a, b in zip(points, points[1:], strict=False)), (
        "no step up: decomposition must add work to the series"
    )


def test_remaining_work_reaches_zero_on_a_completed_run() -> None:
    assert _reduce("clean-run").burndown[-1]["known_points"] == 0


def test_undecomposed_count_falls_to_zero_as_versions_decompose() -> None:
    """It drives the uncertainty band, which must narrow to nothing."""
    counts = [p["undecomposed"] for p in _reduce("clean-run").burndown]
    assert counts[0] > 0 and counts[-1] == 0
    assert counts == sorted(counts, reverse=True), "undecomposed must only ever fall"


def test_issue_sizes_weight_the_series() -> None:
    """Three S issues must not outrank one L (vision §6.2)."""
    state = _reduce("retry-run")
    sizes = [i["size"] for issues in _decomposed_issues(state) for i in [issues]]
    expected_peak = sum(reduce_mod.SIZE_POINTS[s] for s in sizes)
    peak = max(p["known_points"] for p in state.burndown)
    assert peak == expected_peak, (peak, expected_peak, sizes)
    assert peak != len(sizes), "points must differ from a raw count -- sizes carry weight"


def _decomposed_issues(state: reduce_mod.State) -> list[dict[str, Any]]:
    """Every issue the run decomposed, read back off the tree."""
    out: list[dict[str, Any]] = []
    for phase in state.tree:
        for version in phase.get("children") or []:
            for step in version.get("children") or []:
                for issue in step.get("children") or []:
                    out.append({"size": (issue.get("data") or {}).get("size", "M")})
    return out


# ── only lifecycle transitions build the tree ────────────────────────────────


def _uploaded_line(issue: str) -> str:
    """One issue.uploaded, scoped to the upload step exactly as upload-issues emits it."""
    return json.dumps(
        {
            "v": 1,
            "ts": "2026-08-03T14:25:00.000Z",
            "run_id": gen_log.RUN_ID,
            "type": "issue.uploaded",
            "emitter": "skill:upload-issues",
            "scope": {
                "phase": "v01",
                "version": "v01.01",
                "step": "upload-issues",
                "issue": issue,
            },
            "status": "ok",
            "data": {"issue": issue, "gh_number": 1, "url": "http://x/1"},
        }
    )


def test_issue_uploaded_does_not_open_a_node() -> None:
    """A non-lifecycle event must never materialize a node.

    issue.uploaded is scoped to step=upload-issues, where no issue node is ever
    opened. Creating one gave it no start and no end, so it stayed "running" for the
    rest of the run.
    """
    lines = gen_log.preset("clean-run").splitlines() + [_uploaded_line("RF-901")]
    state = reduce_mod.reduce(lines, NOW)

    def walk(nodes: list[dict[str, Any]]) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        for node in nodes:
            out.append(node)
            out.extend(walk(node.get("children") or []))
        return out

    assert not [n for n in walk(state.tree) if n["id"] == "RF-901"]


def test_a_phantom_node_cannot_capture_the_now_line() -> None:
    """The header's "now" must name real work, not a finished upload.

    Phantom issue nodes sit at the deepest path in the tree, so before the fix they
    outranked every genuinely running node.
    """
    lines = gen_log.preset("clean-run").splitlines() + [_uploaded_line("RF-902")]
    state = reduce_mod.reduce(lines, NOW)
    assert "RF-902" not in (state.current or "")


def test_uploaded_issues_are_still_counted() -> None:
    """Dropping the node must not drop the metric it feeds."""
    base = gen_log.preset("clean-run").splitlines()
    before = reduce_mod.reduce(base, NOW)
    after = reduce_mod.reduce(base + [_uploaded_line("RF-903")], NOW)
    assert after.github["created"] == before.github["created"] + 1


# ── the run id comes off the envelope ────────────────────────────────────────


def test_run_id_is_read_from_the_envelope() -> None:
    """It lives on the envelope, never in data -- reading `data` yielded "" always.

    The dashboard overwrote the field after reducing, so the blank only showed up in
    every other consumer.
    """
    assert _reduce("clean-run").run_id == gen_log.RUN_ID


# ── tests_passing must track the repo, not a high-water mark ─────────────────


def _tests_line(etype: str, passed: int, **extra: Any) -> str:
    scope = {"phase": "v01", "version": "v01.01"}
    if etype == "issue.validate.end":
        scope = {**scope, "step": "execute-issues", "issue": "RF-001"}
    return json.dumps(
        {
            "v": 1,
            "ts": "2026-08-03T14:40:00.000Z",
            "run_id": gen_log.RUN_ID,
            "type": etype,
            "emitter": "skill:review-and-fix-issues",
            "scope": scope,
            "status": "ok",
            "data": {"pytest": {"passed": passed, "failed": 0}, **extra},
        }
    )


def test_a_review_fix_updates_the_suite_size() -> None:
    """A fix adds regression tests; the panel must not stay frozen on the last issue."""
    base = gen_log.preset("clean-run").splitlines()
    before = reduce_mod.reduce(base, NOW).metrics["tests_passing"]
    after = reduce_mod.reduce(
        base + [_tests_line("finding.fixed", before + 4, finding="f1", sha="abc1234")], NOW
    ).metrics["tests_passing"]
    assert after == before + 4


def test_a_hardening_fix_updates_the_suite_size() -> None:
    base = gen_log.preset("clean-run").splitlines()
    before = reduce_mod.reduce(base, NOW).metrics["tests_passing"]
    after = reduce_mod.reduce(
        base + [_tests_line("harden.finding.fixed", before + 7, finding="f1", sha="abc1234")],
        NOW,
    ).metrics["tests_passing"]
    assert after == before + 7


def test_the_suite_size_can_go_down() -> None:
    """It reports the suite now, not the largest it ever was.

    A running maximum would keep reporting a count the repo no longer has the moment
    tests are consolidated, and would never recover.
    """
    base = gen_log.preset("clean-run").splitlines()
    before = reduce_mod.reduce(base, NOW).metrics["tests_passing"]
    assert before > 5
    after = reduce_mod.reduce(
        base + [_tests_line("finding.fixed", 5, finding="f1", sha="abc1234")], NOW
    ).metrics["tests_passing"]
    assert after == 5


def test_a_fix_without_counts_leaves_the_suite_size_alone() -> None:
    """An emitter that omits pytest data must not zero the panel."""
    base = gen_log.preset("clean-run").splitlines()
    before = reduce_mod.reduce(base, NOW).metrics["tests_passing"]
    line = json.dumps(
        {
            "v": 1, "ts": "2026-08-03T14:40:00.000Z", "run_id": gen_log.RUN_ID,
            "type": "finding.fixed", "emitter": "skill:review-and-fix-issues",
            "scope": {"phase": "v01", "version": "v01.01"},
            "status": "ok", "data": {"finding": "f1", "sha": "abc1234"},
        }
    )
    assert reduce_mod.reduce(base + [line], NOW).metrics["tests_passing"] == before


def test_resuming_reopens_a_run_that_was_closed() -> None:
    """A resumed run is running again -- not still finished.

    Resuming exists to continue a run that stopped without closing. Leaving the
    terminal status set would show the rest of the work happening inside a run the
    panel still calls aborted.
    """
    lines = gen_log.preset("clean-run").splitlines() + [
        json.dumps({
            "v": 1, "ts": "2026-08-03T15:00:00.000Z", "run_id": gen_log.RUN_ID,
            "type": "run.aborted", "emitter": "hook:on-stop",
            "scope": {}, "status": "fail", "data": {"reason": "session-stopped"},
        }),
        json.dumps({
            "v": 1, "ts": "2026-08-03T15:05:00.000Z", "run_id": gen_log.RUN_ID,
            "type": "run.resumed", "emitter": "skill:ship-phase",
            "scope": {}, "status": "ok", "data": {"gap_s": 300},
        }),
    ]
    state = reduce_mod.reduce(lines, NOW)
    assert state.status == "running"
    assert state.ended is None


def test_resuming_still_excludes_the_idle_gap() -> None:
    """Reopening must not cost the idle accounting it also carries."""
    lines = gen_log.preset("clean-run").splitlines() + [
        json.dumps({
            "v": 1, "ts": "2026-08-03T15:05:00.000Z", "run_id": gen_log.RUN_ID,
            "type": "run.resumed", "emitter": "skill:ship-phase",
            "scope": {}, "status": "ok", "data": {"gap_s": 600},
        }),
    ]
    assert reduce_mod.reduce(lines, NOW).idle_s >= 600


# ── #113: an unclosed node must not accrue against wall-clock ────────────────


def _unclosed_step_log() -> list[str]:
    """A clean run with one ``step.end`` missing — what 7 of 15 versions looked
    like in run-20260815-213849, where a skill stopped emitting the pair."""
    return [
        line for line in gen_log.preset("clean-run").splitlines()
        if not (
            json.loads(line).get("type") == "step.end"
            and json.loads(line)["scope"].get("step") == "execute-issues"
        )
    ]


def _step_nodes(state: reduce_mod.State) -> list[dict[str, Any]]:
    found: list[dict[str, Any]] = []

    def walk(node: dict[str, Any]) -> None:
        if node["kind"] == "step":
            found.append(node)
        for child in node.get("children") or []:
            walk(child)

    for phase in state.as_dict()["tree"]:
        walk(phase)
    return found


def test_an_unclosed_step_never_outlasts_its_run() -> None:
    """The defect in #113: a step whose version has ended cannot still be running.

    Before the fix one ``execute-issues`` node reported 620,225 s (172 h) inside a
    14,702 s (4.1 h) run, because an unclosed node measured to ``now``.
    """
    state = reduce_mod.reduce(_unclosed_step_log(), NOW)
    assert state.elapsed_s > 0
    for node in _step_nodes(state):
        assert node["elapsed_s"] <= state.elapsed_s, (
            f"step {node['id']} reports {node['elapsed_s']}s "
            f"inside a run of {state.elapsed_s}s"
        )


def test_a_finished_run_reduces_the_same_whenever_you_look_at_it() -> None:
    """The sharper statement of the same bug, and the one that would have caught it.

    A finished run is a fixed set of facts. If any figure moves because the log sat
    on disk longer, the reducer is reading a clock it has no business reading.
    """
    lines = _unclosed_step_log()
    immediately = reduce_mod.reduce(lines, NOW).as_dict()
    much_later = reduce_mod.reduce(lines, NOW + timedelta(days=365)).as_dict()
    assert immediately == much_later


def test_a_running_node_still_grows_while_the_run_is_open() -> None:
    """The bound must not flatten a live run — an open node measures to ``now``,
    which is the behaviour the header's elapsed clock depends on."""
    lines = [
        line for line in gen_log.preset("clean-run").splitlines()
        if json.loads(line).get("type") not in {"run.end", "phase.end", "version.end"}
    ]
    early = reduce_mod.reduce(lines, NOW)
    later = reduce_mod.reduce(lines, NOW + timedelta(hours=1))
    assert later.elapsed_s > early.elapsed_s

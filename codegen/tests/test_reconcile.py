"""TRK — the reconciliation pass: did the skills record what the hooks saw?

Architecture §10.4. The rate is only meaningful if its denominator is. Hooks are
context-free, so every ``pytest`` on the machine looks alike to them; these tests pin
which ones a skill is actually answerable for.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from tracker import paths, reconcile

RUN_ID = "run-20260803-120000"


def _write(runs: Path, events: list[dict[str, Any]]) -> str:
    directory = paths.run_dir(RUN_ID)
    directory.mkdir(parents=True, exist_ok=True)
    paths.events_path(RUN_ID).write_text(
        "\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8"
    )
    return RUN_ID


def _event(etype: str, **kwargs: Any) -> dict[str, Any]:
    return {
        "v": 1,
        "ts": "2026-08-03T12:00:00.000Z",
        "run_id": RUN_ID,
        "type": etype,
        "emitter": kwargs.pop("emitter", "skill:execute-issues"),
        "scope": kwargs.pop("scope", {}),
        "data": kwargs.pop("data", {}),
    }


def _pytest_run() -> dict[str, Any]:
    return _event(
        "tool.used",
        emitter="hook:on-tool-use",
        data={"tool": "Bash", "program": "echo", "argv_len": 6, "pytest": True},
    )


def _issue_scope(issue: str) -> dict[str, str]:
    return {"phase": "v01", "version": "v01.01", "step": "execute-issues", "issue": issue}


def test_an_issue_that_validated_and_emitted_is_compliant(isolated_runs_dir: Path) -> None:
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.start", scope=_issue_scope("RF-001")),
            _pytest_run(),
            _event("issue.validate.end", scope=_issue_scope("RF-001")),
            _event("issue.end", scope=_issue_scope("RF-001")),
        ],
    )
    report = reconcile.reconcile(run, git_shas=set())
    assert (report.validate_observed, report.validate_emitted) == (1, 1)
    assert report.emit_rate == 1.0


def test_an_issue_that_validated_without_emitting_is_caught(isolated_runs_dir: Path) -> None:
    """The failure this check exists for: the skill ran pytest and recorded nothing."""
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.start", scope=_issue_scope("RF-001")),
            _pytest_run(),
            _event("issue.end", scope=_issue_scope("RF-001")),
        ],
    )
    report = reconcile.reconcile(run, git_shas=set())
    assert report.emit_rate == 0.0
    assert report.unemitted_issues == ["RF-001"]


def test_pytest_outside_an_issue_is_not_charged_to_a_skill(isolated_runs_dir: Path) -> None:
    """Baselines, review checks and ad-hoc shells have no issue.validate.end to match.

    Counting them made the rate a measure of how much debugging happened.
    """
    run = _write(
        isolated_runs_dir,
        [
            _pytest_run(),  # green baseline, before any issue
            _event("issue.start", scope=_issue_scope("RF-001")),
            _pytest_run(),
            _event("issue.validate.end", scope=_issue_scope("RF-001")),
            _event("issue.end", scope=_issue_scope("RF-001")),
            _pytest_run(),  # the review step verifying its own fix
            _pytest_run(),  # a maintainer at a shell
        ],
    )
    report = reconcile.reconcile(run, git_shas=set())
    assert report.emit_rate == 1.0, "out-of-band runs must not drag the rate down"
    assert report.out_of_band_validations == 3
    assert any("no issue open" in note for note in report.notes)


def test_repeated_pytest_within_one_issue_counts_once(isolated_runs_dir: Path) -> None:
    """One validation legitimately runs pytest several times -- app suite, tracker, a
    collection check. The question is whether the issue was recorded at all."""
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.start", scope=_issue_scope("RF-001")),
            _pytest_run(),
            _pytest_run(),
            _pytest_run(),
            _event("issue.validate.end", scope=_issue_scope("RF-001")),
            _event("issue.end", scope=_issue_scope("RF-001")),
        ],
    )
    report = reconcile.reconcile(run, git_shas=set())
    assert (report.validate_observed, report.validate_emitted) == (1, 1)


def test_each_issue_is_judged_separately(isolated_runs_dir: Path) -> None:
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.start", scope=_issue_scope("RF-001")),
            _pytest_run(),
            _event("issue.validate.end", scope=_issue_scope("RF-001")),
            _event("issue.end", scope=_issue_scope("RF-001")),
            _event("issue.start", scope=_issue_scope("RF-002")),
            _pytest_run(),
            _event("issue.end", scope=_issue_scope("RF-002")),
        ],
    )
    report = reconcile.reconcile(run, git_shas=set())
    assert (report.validate_observed, report.validate_emitted) == (2, 1)
    assert report.unemitted_issues == ["RF-002"]


def test_a_run_with_no_hook_events_has_no_rate(isolated_runs_dir: Path) -> None:
    """No hook floor means no measurement -- not a score of zero."""
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.start", scope=_issue_scope("RF-001")),
            _event("issue.end", scope=_issue_scope("RF-001")),
        ],
    )
    assert reconcile.reconcile(run, git_shas=set()).emit_rate is None


def test_commits_claimed_are_checked_against_git(isolated_runs_dir: Path) -> None:
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.commit", scope=_issue_scope("RF-001"), data={"sha": "aaa1111"}),
            _event("finding.fixed", scope={"phase": "v01", "version": "v01.01"},
                   data={"sha": "bbb2222"}),
        ],
    )
    report = reconcile.reconcile(run, git_shas={"aaa1111"})
    assert report.commits_claimed == 2
    assert report.commits_in_git == 1
    assert report.missing_in_git == ["bbb2222"]


def test_two_events_sharing_one_commit_do_not_lower_the_rate(
    isolated_runs_dir: Path,
) -> None:
    """Both sides count distinct shas.

    Counting claim *events* against distinct shas in git made the rate fall whenever two
    events legitimately shared a commit -- three issues batched into one, or a review fix
    landing alongside its issue -- while `missing_in_git` stayed empty, so the report
    showed a shortfall it could not explain.
    """
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.commit", scope=_issue_scope("RF-001"), data={"sha": "aaa1111"}),
            _event("issue.commit", scope=_issue_scope("RF-002"), data={"sha": "aaa1111"}),
            _event("issue.commit", scope=_issue_scope("RF-003"), data={"sha": "aaa1111"}),
        ],
    )
    report = reconcile.reconcile(run, git_shas={"aaa1111"})
    assert (report.commits_claimed, report.commits_in_git) == (1, 1)
    assert report.commit_rate == 1.0
    assert report.missing_in_git == []


def test_a_genuinely_missing_commit_still_lowers_the_rate(
    isolated_runs_dir: Path,
) -> None:
    """The fix must not make the check unable to fail."""
    run = _write(
        isolated_runs_dir,
        [
            _event("issue.commit", scope=_issue_scope("RF-001"), data={"sha": "aaa1111"}),
            _event("issue.commit", scope=_issue_scope("RF-002"), data={"sha": "ghost99"}),
        ],
    )
    report = reconcile.reconcile(run, git_shas={"aaa1111"})
    assert (report.commits_claimed, report.commits_in_git) == (2, 1)
    assert report.commit_rate == 0.5
    assert report.missing_in_git == ["ghost99"]

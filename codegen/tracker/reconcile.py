"""Compliance report — did the skills emit what they were told to?

Architecture §10.4. Whether a model followed an emit instruction is a property of a
*run*, not of code, so this cannot be a unit test. It is a post-run analysis that
reports a **rate**, and it **never fails a build**: a falling rate is a signal that
skill files have grown too long, which is the observer-effect risk the vision names —
not a reason to block anything.

Two independent checks:

* hooks saw a tool call; did the skill emit the matching semantic event?
* the log claims a commit; does ``git log`` contain it, and vice versa?

The emit check is scoped **per issue, and only while an issue is open**. Hooks are
context-free -- ``tool.used`` carries no scope -- so a naive count charges the skill for
every ``pytest`` on the machine: the orchestrator's own green baseline, the review step's
verification, a maintainer debugging the tracker. None of those has an
``issue.validate.end`` to match, and counting them made the rate a measure of how much
debugging happened rather than of skill compliance. Out-of-band runs are reported as a
note instead, so the information is kept without corrupting the rate.

Per *issue* rather than per *invocation* because one validation legitimately runs pytest
more than once (the application suite, a collection check, the tracker's own suite). The
question worth asking is "did the skill record that it validated this issue at all?",
which is what the rate now answers.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from typing import Any

from tracker import paths


@dataclass
class Report:
    run_id: str
    #: Issues that ran pytest while open -- the denominator of the emit rate.
    validate_observed: int = 0
    #: How many of those also emitted at least one ``issue.validate.end``.
    validate_emitted: int = 0
    #: pytest runs seen with no issue open. Reported, never charged to a skill.
    out_of_band_validations: int = 0
    commits_claimed: int = 0
    commits_in_git: int = 0
    missing_in_git: list[str] = field(default_factory=list)
    missing_in_log: list[str] = field(default_factory=list)
    unemitted_issues: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def emit_rate(self) -> float | None:
        if not self.validate_observed:
            return None
        return round(min(1.0, self.validate_emitted / self.validate_observed), 4)

    @property
    def commit_rate(self) -> float | None:
        if not self.commits_claimed:
            return None
        return round(min(1.0, self.commits_in_git / self.commits_claimed), 4)

    def to_markdown(self) -> str:
        lines = [
            f"# Reconciliation — {self.run_id}",
            "",
            "A measurement, never a gate. A falling rate means the skill files have grown",
            "too long for their emit instructions to survive — the observer effect, showing up.",
            "",
            "| Check | Observed | Emitted | Rate |",
            "|---|---|---|---|",
            f"| issues validated vs `issue.validate.end` | {self.validate_observed} | "
            f"{self.validate_emitted} | {_pct(self.emit_rate)} |",
            f"| commits claimed vs in `git log` | {self.commits_claimed} | "
            f"{self.commits_in_git} | {_pct(self.commit_rate)} |",
            "",
        ]
        if self.missing_in_git:
            lines += ["**Claimed in the log, absent from git:**", ""]
            lines += [f"- `{sha}`" for sha in self.missing_in_git] + [""]
        if self.missing_in_log:
            lines += ["**In git, absent from the log:**", ""]
            lines += [f"- `{sha}`" for sha in self.missing_in_log] + [""]
        lines += self.notes
        return "\n".join(lines) + "\n"


def _pct(value: float | None) -> str:
    return "—" if value is None else f"{value * 100:.1f}%"


def _events(run_id: str) -> list[dict[str, Any]]:
    path = paths.events_path(run_id)
    if not path.is_file():
        return []
    out: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def _git_shas(limit: int = 500) -> set[str]:
    try:
        result = subprocess.run(
            ["git", "log", f"-{limit}", "--format=%h"],
            capture_output=True, text=True, timeout=15,
            cwd=paths.codegen_root().parent,
        )
        return set(result.stdout.split()) if result.returncode == 0 else set()
    except (OSError, subprocess.SubprocessError):
        return set()


def reconcile(run_id: str, git_shas: set[str] | None = None) -> Report:
    """Compare the hook floor, the skill events and git. Never raises on mismatch."""
    events = _events(run_id)
    report = Report(run_id=run_id)

    open_issue: str | None = None
    validated: set[str] = set()  # issues that ran pytest while open
    emitted: set[str] = set()  # issues that recorded issue.validate.end

    for event in events:
        etype = event.get("type")
        data = event.get("data") or {}
        scope = event.get("scope") or {}
        if etype == "issue.start":
            open_issue = str(scope.get("issue") or "") or None
        elif etype == "issue.end":
            open_issue = None
        elif etype == "tool.used":
            program = str(data.get("program", ""))
            sub = str(data.get("subcommand", ""))
            # `pytest` is the hook's own flag and covers compound commands; the
            # program/subcommand fallbacks keep older logs readable.
            if data.get("pytest") or program.startswith("pytest") or sub == "pytest":
                if open_issue:
                    validated.add(open_issue)
                else:
                    # No issue open: a baseline, a review-step check, or a maintainer
                    # at a shell. No skill owes an emit for these.
                    report.out_of_band_validations += 1
        elif etype == "issue.validate.end":
            issue = str(scope.get("issue") or "") or open_issue
            if issue:
                emitted.add(issue)

    shas_in_log = {
        str((e.get("data") or {}).get("sha", ""))
        for e in events
        if e.get("type") in {"issue.commit", "finding.fixed", "harden.finding.fixed"}
    } - {""}

    # Distinct shas on both sides. Counting *events* here and distinct shas in git made
    # the rate drop whenever two events legitimately shared one commit -- a batched
    # issue, or a review fix landing alongside the issue it belongs to -- while
    # `missing_in_git` stayed empty, so the report showed a shortfall it could not name.
    report.commits_claimed = len(shas_in_log)

    report.validate_observed = len(validated)
    report.validate_emitted = len(validated & emitted)
    report.unemitted_issues = sorted(validated - emitted)
    actual = git_shas if git_shas is not None else _git_shas()
    if actual:
        report.commits_in_git = len(shas_in_log & actual)
        report.missing_in_git = sorted(shas_in_log - actual)
    else:
        report.notes.append("_git history unavailable; commit check skipped._")
        report.commits_in_git = report.commits_claimed

    if report.unemitted_issues:
        report.notes.append(
            "_Validated by hooks but never recorded by the skill: "
            + ", ".join(f"`{issue}`" for issue in report.unemitted_issues)
            + "._"
        )
    if report.out_of_band_validations:
        report.notes.append(
            f"_{report.out_of_band_validations} pytest run(s) happened with no issue open "
            "(baselines, review checks, ad-hoc shells). Not charged to any skill._"
        )
    return report


def _main(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="tracker.reconcile")
    parser.add_argument("run_id", nargs="?")
    args = parser.parse_args(argv)

    run_id = args.run_id
    if not run_id:
        pointer = paths.current_pointer()
        run_id = pointer.read_text(encoding="utf-8").strip() if pointer.is_file() else ""
    if not run_id:
        print("no run id given and no active run")
        return 1

    report = reconcile(run_id)
    out = paths.var_root() / f"reconcile-{run_id}.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(report.to_markdown(), encoding="utf-8")
    print(f"{out}: emit {_pct(report.emit_rate)}, commits {_pct(report.commit_rate)}")
    return 0  # never non-zero: this is a measurement, not a gate


if __name__ == "__main__":  # pragma: no cover
    import sys

    sys.exit(_main(sys.argv[1:]))

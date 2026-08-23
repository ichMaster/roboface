"""TRK-016 / TRK-017 / TRK-018 — hooks and the compliance report.

Hooks run inside the session they observe, so the tests are mostly about what they
must *not* do: raise, exit non-zero, print to stdout, or record a command string.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import pytest

from hooks import on_stop, on_tool_use
from tracker import emit, paths, reconcile

HOOKS = paths.codegen_root() / "hooks"


@pytest.fixture
def active_run(isolated_runs_dir: Path) -> str:
    run_id = "run-20260803-142012"
    paths.run_dir(run_id).mkdir(parents=True, exist_ok=True)
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    return run_id


def _events(run_id: str) -> list[dict[str, Any]]:
    path = paths.events_path(run_id)
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x.strip()]


def _run_hook(script: str, payload: Any, run_id_dir: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, CODEGEN_RUNS_DIR=str(run_id_dir))
    return subprocess.run(
        [sys.executable, str(HOOKS / script)],
        input=payload if isinstance(payload, str) else json.dumps(payload),
        env=env, capture_output=True, text=True, timeout=30,
    )


BASH_PAYLOAD = {
    "tool_name": "Bash",
    "tool_input": {"command": "pytest codegen/tests -q"},
    "tool_response": {"exit_code": 0, "duration_ms": 1200},
}


def test_a_bash_call_becomes_a_tool_used_event(active_run: str) -> None:
    result = _run_hook("on_tool_use.py", BASH_PAYLOAD, paths.runs_root())
    assert result.returncode == 0
    events = _events(active_run)
    assert [e["type"] for e in events] == ["tool.used"]
    assert events[0]["data"]["program"] == "pytest"


@pytest.mark.parametrize(
    "payload",
    ["", "   ", "{not json", "[]", '"a string"', json.dumps({"tool_name": "Read"})],
    ids=["empty", "blank", "broken", "list", "string", "uninteresting"],
)
def test_bad_or_uninteresting_input_exits_zero_and_writes_nothing(
    active_run: str, payload: str
) -> None:
    result = _run_hook("on_tool_use.py", payload, paths.runs_root())
    assert result.returncode == 0
    assert result.stdout == ""
    assert _events(active_run) == []


def test_the_raw_command_never_reaches_disk(active_run: str) -> None:
    """A command line can carry an API key, so only its shape is recorded."""
    secret = "sk-ant-" + "A" * 30
    payload = {
        "tool_name": "Bash",
        "tool_input": {"command": f"ANTHROPIC_API_KEY={secret} python agent/agent.py --live"},
        "tool_response": {"exit_code": 0},
    }
    _run_hook("on_tool_use.py", payload, paths.runs_root())
    written = paths.events_path(active_run).read_text(encoding="utf-8")
    assert secret not in written
    assert "agent/agent.py" not in written, "the command line itself must not be recorded"


def test_file_tools_record_only_the_basename(active_run: str) -> None:
    payload = {
        "tool_name": "Write",
        "tool_input": {"file_path": "/Users/someone/secret-project/server/main.py",
                       "content": "an api key could be in here"},
        "tool_response": {},
    }
    _run_hook("on_tool_use.py", payload, paths.runs_root())
    written = paths.events_path(active_run).read_text(encoding="utf-8")
    assert "main.py" in written
    assert "secret-project" not in written
    assert "an api key" not in written


def test_the_hook_is_fast_enough_to_run_on_every_tool_call(active_run: str) -> None:
    """It runs on every call; a slow hook taxes the whole pipeline. Budget: 50ms p95."""
    durations: list[float] = []
    for _ in range(20):
        started = time.perf_counter()
        on_tool_use.build_data(BASH_PAYLOAD)
        durations.append(time.perf_counter() - started)
    durations.sort()
    assert durations[int(len(durations) * 0.95) - 1] < 0.05


def test_summarise_bash_keeps_shape_not_content() -> None:
    summary = on_tool_use.summarise_bash("git commit -m 'a message with a secret'")
    assert summary == {"program": "git", "subcommand": "commit", "argv_len": 4,
                       "pytest": False}


# ── the Stop hook ────────────────────────────────────────────────────────────


def test_stop_does_not_close_an_open_run(active_run: str) -> None:
    """Stop fires at the end of every assistant turn, not at session end.

    A /ship-phase run spans many turns, so writing a terminal event here closed the
    run at the first turn boundary and every later event landed in a log that already
    claimed to be finished.
    """
    emit.emit("run.start", emitter="skill:ship-phase", data={
        "command": "/ship-phase v01", "plan": ["v01.01"],
        "baseline": {"tests": 0, "mypy_errors": 0},
        "git": {"branch": "b", "head_sha": "s", "remote": "o"},
    })
    before = _events(active_run)
    result = _run_hook("on_stop.py", "{}", paths.runs_root())
    assert result.returncode == 0
    assert _events(active_run) == before, "the hook must not write to the run log"
    assert on_stop.run_is_open(active_run) is True


def test_an_unfinished_run_stays_detectable_as_unfinished(active_run: str) -> None:
    """The signal the hook used to destroy: no terminator means "still open".

    tracker.run.pending keys the resume-or-supersede prompt on exactly this, so a
    terminal event written per turn made a dead run look deliberately aborted.
    """
    emit.emit("run.start", emitter="skill:ship-phase", data={
        "command": "/ship-phase v01", "plan": ["v01.01"],
        "baseline": {"tests": 0, "mypy_errors": 0},
        "git": {"branch": "b", "head_sha": "s", "remote": "o"},
    })
    for _ in range(3):
        _run_hook("on_stop.py", "{}", paths.runs_root())
    assert on_stop.run_is_open(active_run) is True


def test_stop_does_not_touch_a_finished_run(active_run: str) -> None:
    """A spurious event would make a clean run look like something happened after it."""
    emit.emit("run.start", emitter="skill:ship-phase", data={
        "command": "/ship-phase v01", "plan": [],
        "baseline": {"tests": 0, "mypy_errors": 0},
        "git": {"branch": "b", "head_sha": "s", "remote": "o"},
    })
    emit.emit("run.end", emitter="skill:ship-phase", status="ok",
              data={"versions_done": 0, "issues_done": 0})
    before = _events(active_run)
    _run_hook("on_stop.py", "{}", paths.runs_root())
    assert _events(active_run) == before


def test_stop_is_silent_when_there_is_no_run(isolated_runs_dir: Path) -> None:
    result = _run_hook("on_stop.py", "{}", paths.runs_root())
    assert result.returncode == 0 and result.stdout == ""


def test_run_is_open_treats_a_torn_tail_as_still_open(active_run: str) -> None:
    path = paths.events_path(active_run)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('{"v":1,"ts":"2026-08-03T14:2', encoding="utf-8")
    assert on_stop.run_is_open(active_run) is True


# ── reconciliation ───────────────────────────────────────────────────────────


def _seed(run_id: str, validations: int, observed: int, shas: list[str]) -> None:
    """``observed`` issues each ran pytest; the first ``validations`` of them emitted.

    Compliance is measured per issue, and only while one is open -- hooks are
    context-free, so a pytest run outside an issue belongs to no skill.
    """
    for index in range(observed):
        scope = {"phase": "v01", "version": "v01.01", "step": "execute-issues",
                 "issue": f"RF-{index + 1:03d}"}
        emit.emit("issue.start", emitter="skill:execute-issues", status="ok", scope=scope,
                  data={"size": "M", "area": "games"})
        emit.emit("tool.used", emitter="hook:on-tool-use", status="ok",
                  data={"tool": "Bash", "program": "pytest", "argv_len": 3, "pytest": True})
        if index < validations:
            emit.emit("issue.validate.end", emitter="skill:execute-issues", status="ok",
                      scope=scope,
                      data={"attempt": 1, "pytest": {"passed": 1, "failed": 0},
                            "mypy": {"errors": 0}})
        emit.emit("issue.end", emitter="skill:execute-issues", status="ok", scope=scope,
                  data={"attempts": 1})

    commit_scope = {"phase": "v01", "version": "v01.01", "step": "execute-issues",
                    "issue": "RF-001"}
    for sha in shas:
        emit.emit("issue.commit", emitter="skill:execute-issues", status="ok",
                  scope=commit_scope, data={"sha": sha, "files": ["x.py"]})


def test_full_compliance_reports_one_hundred_percent(active_run: str) -> None:
    _seed(active_run, validations=3, observed=3, shas=["aaa1111", "bbb2222"])
    report = reconcile.reconcile(active_run, git_shas={"aaa1111", "bbb2222"})
    assert report.emit_rate == 1.0
    assert report.commit_rate == 1.0
    assert report.missing_in_git == []


def test_a_missing_emit_is_reported_and_named(active_run: str) -> None:
    """The gap the reconciliation exists to find: hooks saw it, the skill did not emit."""
    _seed(active_run, validations=1, observed=4, shas=[])
    report = reconcile.reconcile(active_run, git_shas=set())
    assert report.emit_rate == 0.25
    assert "never recorded by the skill" in report.to_markdown()
    assert report.unemitted_issues == ["RF-002", "RF-003", "RF-004"]


def test_a_commit_claimed_but_absent_from_git_is_flagged(active_run: str) -> None:
    _seed(active_run, validations=1, observed=1, shas=["real123", "ghost99"])
    report = reconcile.reconcile(active_run, git_shas={"real123"})
    assert report.missing_in_git == ["ghost99"]
    assert "ghost99" in report.to_markdown()


def test_reconciliation_is_never_a_gate(active_run: str) -> None:
    """It reports a rate; it must not fail anything, even at zero compliance."""
    _seed(active_run, validations=0, observed=5, shas=[])
    env = dict(os.environ, CODEGEN_RUNS_DIR=str(paths.runs_root()))
    result = subprocess.run(
        [sys.executable, "-m", "tracker.reconcile", active_run],
        cwd=paths.codegen_root(), env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0, "a measurement must never exit non-zero"
    assert "0.0%" in result.stdout


def test_settings_json_holds_matchers_and_commands_only() -> None:
    """Architecture §7: the registration is a pointer, never logic.

    Shipped as a template inside codegen/ rather than an enabled .claude/settings.json:
    a committed registration fires on every tool call in every session for anyone who
    clones the repo, and a broken one BLOCKS those calls. Opt in deliberately.
    """
    settings_path = paths.codegen_root() / "hooks" / "settings.hooks.json"
    settings = json.loads(settings_path.read_text(encoding="utf-8"))

    commands = [
        entry["command"]
        for group in settings.get("hooks", {}).values()
        for matcher in group
        for entry in matcher.get("hooks", [])
    ]
    assert commands, "no hooks registered"
    for command in commands:
        # $CLAUDE_PROJECT_DIR, not a relative path. A relative path resolves against the
        # session's cwd, which is not the repo root whenever a command has cd'd -- the
        # hook then silently fails with "can't open file". Found the hard way: this
        # registration broke live, in the session that wrote it.
        assert "$CLAUDE_PROJECT_DIR" in command, command
        assert "/codegen/hooks/" in command, command
        assert not command.startswith("python3 codegen/"), (
            "relative hook path: breaks whenever cwd is not the repo root"
        )
        # A command is an invocation, never a program: no pipes, no chaining, no logic.
        assert not any(token in command for token in ("&&", "||", ";", "|", "`")), command

    allowed = {"hooks", "matcher", "command", "type", "PostToolUse", "Stop"}
    for group in settings.get("hooks", {}).values():
        for matcher in group:
            assert set(matcher) <= allowed, set(matcher) - allowed


# ── pytest detection anywhere in the command (architecture §10.4) ────────────


@pytest.mark.parametrize(
    "command",
    [
        "pytest -q",
        ".venv/bin/pytest -q",
        "cd repo && .venv/bin/pytest",
        "python3 -m pytest tests/",
        'echo "=== tests ===" && ../.venv/bin/pytest -c codegen/pyproject.toml',
        "py.test",
    ],
)
def test_pytest_is_detected_anywhere_in_the_command(command: str) -> None:
    """argv[0] alone misses most real invocations, undercounting validated issues."""
    assert on_tool_use.summarise_bash(command)["pytest"] is True


@pytest.mark.parametrize(
    "command",
    ["git status", "gh issue list", "python3 -m tracker.emit issue.start", "ls -la"],
)
def test_non_pytest_commands_are_not_flagged(command: str) -> None:
    assert on_tool_use.summarise_bash(command)["pytest"] is False


def test_the_command_text_is_still_never_recorded() -> None:
    """The §8 rule holds: a bool is added, not any part of the command line."""
    command = "pytest --token=SECRET-abc123 -q"
    summary = on_tool_use.summarise_bash(command)
    assert summary["pytest"] is True
    assert "SECRET-abc123" not in json.dumps(summary)

"""TRK-003 / TRK-004 — the emitter's properties, and redaction.

These are property tests rather than scenario tests: the emitter's contract is a set
of guarantees that must hold for *every* input, and the one that matters most —
never raising — is only meaningful if it is checked against inputs designed to break it.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from tracker import emit, paths, redact

EMITTER = "skill:execute-issues"


@pytest.fixture
def active_run(isolated_runs_dir: Path) -> str:
    """A run the emitter will attribute events to."""
    run_id = "run-20260803-142012"
    (isolated_runs_dir / run_id).mkdir(parents=True, exist_ok=True)
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    return run_id


def _events(run_id: str) -> list[dict[str, Any]]:
    path = paths.events_path(run_id)
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


# ── the never-raise guarantee ────────────────────────────────────────────────


@pytest.mark.parametrize(
    "label,setup",
    [
        ("no active run", lambda d: paths.current_pointer().unlink(missing_ok=True)),
        ("runs dir removed", lambda d: __import__("shutil").rmtree(d)),
        ("current is a directory", lambda d: paths.current_pointer().mkdir()),
        ("current is empty", lambda d: paths.current_pointer().write_text("")),
    ],
)
def test_emit_never_raises(
    isolated_runs_dir: Path, label: str, setup: Any, capsys: pytest.CaptureFixture[str]
) -> None:
    setup(isolated_runs_dir)
    emit.emit("phase.start", emitter=EMITTER, scope={"phase": "v01"})  # must not raise
    captured = capsys.readouterr()
    assert captured.out == "" and captured.err == "", f"{label} produced output"


def test_emit_never_raises_on_unserialisable_payload(active_run: str) -> None:
    class Opaque:
        pass

    emit.emit(  # must not raise
        "phase.end",
        emitter=EMITTER,
        scope={"phase": "v01"},
        status="ok",
        data={"versions": Opaque()},
    )


def test_emit_never_raises_when_the_log_is_unwritable(active_run: str) -> None:
    log = paths.events_path(active_run)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.touch()
    log.chmod(0o400)
    try:
        emit.emit("phase.start", emitter=EMITTER, scope={"phase": "v01"})  # must not raise
    finally:
        log.chmod(0o644)


def test_an_invalid_event_is_not_written_and_the_reason_is_recorded(active_run: str) -> None:
    """A missing required scope key must be caught before the line reaches disk."""
    emit.emit(
        "issue.start", emitter=EMITTER, scope={"phase": "v01"},
        data={"size": "M", "area": "x"},
    )
    assert _events(active_run) == []
    errors = (paths.var_root() / emit.ERROR_LOG_NAME).read_text(encoding="utf-8")
    assert "scope.version" in errors


# ── write discipline ─────────────────────────────────────────────────────────


def test_exactly_one_write_call_per_event(active_run: str, monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[bytes] = []
    real_write = os.write

    def counting_write(fd: int, payload: bytes) -> int:
        calls.append(payload)
        return real_write(fd, payload)

    monkeypatch.setattr(os, "write", counting_write)
    emit.emit("phase.start", emitter=EMITTER, scope={"phase": "v01"})
    assert len(calls) == 1
    assert calls[0].endswith(b"\n")


def test_a_huge_payload_is_truncated_not_split(active_run: str) -> None:
    emit.emit(
        "gate.blocked",
        emitter=EMITTER,
        scope={"phase": "v01", "version": "v01.01", "step": "release"},
        status="fail",
        data={"gate": "release", "reason": "x" * 100_000},
    )
    raw = paths.events_path(active_run).read_bytes()
    assert raw.count(b"\n") == 1, "the event must remain one line"
    assert len(raw) <= emit.MAX_LINE_BYTES
    assert _events(active_run)[0]["data"]["_truncated"] is True


def test_events_are_appended_never_overwritten(active_run: str) -> None:
    for _ in range(5):
        emit.emit("phase.start", emitter=EMITTER, scope={"phase": "v01"})
    assert len(_events(active_run)) == 5


def test_concurrent_writers_produce_no_corrupt_lines(active_run: str, tmp_path: Path) -> None:
    """Architecture §4.2's measurement, as an executable test."""
    script = tmp_path / "writer.py"
    script.write_text(
        "import sys\n"
        f"sys.path.insert(0, {str(paths.codegen_root())!r})\n"
        "from tracker import emit\n"
        "for i in range(60):\n"
        "    emit.emit('phase.start', emitter='skill:ship-phase', scope={'phase': 'v01'})\n",
        encoding="utf-8",
    )
    env = dict(os.environ, CODEGEN_RUNS_DIR=str(paths.runs_root()))
    procs = [
        subprocess.Popen([sys.executable, str(script)], env=env, stdout=subprocess.DEVNULL)
        for _ in range(8)
    ]
    for proc in procs:
        assert proc.wait(timeout=60) == 0

    lines = paths.events_path(active_run).read_text(encoding="utf-8").splitlines()
    assert len(lines) == 8 * 60
    for line in lines:
        json.loads(line)  # raises on a torn line


# ── redaction ────────────────────────────────────────────────────────────────

# Built from parts on purpose. Written as literals, these tripped GitHub's own
# secret scanner and blocked the push -- which says the shapes are realistic, but a
# test fixture must never itself look like a live credential.
_A = "A" * 30
SECRETS = [
    "sk-ant-api03-" + _A,
    "ghp_" + _A,
    "github_pat_" + _A,
    "AKIA" + "B" * 16,
    "xox" + "b-" + "1" * 12 + "-" + _A,
    "ANTHROPIC_API_KEY=sk-ant-" + _A,
    "api_key: " + _A,
    "Authorization: Bearer " + _A,
    "password=" + _A,
    "TOKEN = " + _A,
]


@pytest.mark.parametrize("secret", SECRETS, ids=lambda s: s.split("=")[0][:14])
def test_secrets_never_reach_disk(active_run: str, secret: str) -> None:
    emit.emit(
        "gate.blocked",
        emitter=EMITTER,
        scope={"phase": "v01", "version": "v01.01", "step": "execute-issues"},
        status="fail",
        data={"gate": "execute", "reason": f"command failed: {secret}"},
    )
    written = paths.events_path(active_run).read_text(encoding="utf-8")
    payload = secret.split("=", 1)[-1].split(": ", 1)[-1].strip()
    assert payload not in written, f"leaked: {secret}"
    assert redact.MARKER in written


@pytest.mark.parametrize("depth", [1, 3, 6])
def test_secrets_are_redacted_at_any_nesting_depth(active_run: str, depth: int) -> None:
    payload: Any = "sk-ant-api03-BBBBBBBBBBBBBBBBBBBBBBBB"
    for _ in range(depth):
        payload = {"nested": [payload]}
    emit.emit(
        "issue.commit",
        emitter=EMITTER,
        scope={"phase": "v01", "version": "v01.01", "step": "execute-issues", "issue": "RF-001"},
        status="ok",
        data={"sha": "abc1234", "files": [], "extra": payload},
    )
    assert "sk-ant-api03-BBBB" not in paths.events_path(active_run).read_text(encoding="utf-8")


def test_prose_containing_the_word_token_is_not_mangled() -> None:
    """No false positives: a finding title may legitimately discuss tokens."""
    text = "the token bucket refills every second"
    assert redact.redact_text(text) == text


def test_redaction_keeps_the_key_name_so_the_event_stays_readable() -> None:
    out = redact.redact_text("ANTHROPIC_API_KEY=sk-ant-aaaaaaaaaaaaaaaaaaaa")
    assert out.startswith("ANTHROPIC_API_KEY=")
    assert redact.MARKER in out
    assert "sk-ant" not in out


# ── CLI ──────────────────────────────────────────────────────────────────────


def test_cli_writes_an_event(active_run: str) -> None:
    env = dict(os.environ, CODEGEN_RUNS_DIR=str(paths.runs_root()))
    result = subprocess.run(
        [
            sys.executable, "-m", "tracker.emit", "version.start",
            "--emitter", "skill:ship-phase",
            "--scope", "phase=v01,version=v01.01",
        ],
        cwd=paths.codegen_root(), env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stderr
    assert _events(active_run)[0]["type"] == "version.start"


def test_cli_exits_zero_even_on_bad_input(active_run: str) -> None:
    """A hook calls this; a non-zero exit could disturb the session being observed."""
    env = dict(os.environ, CODEGEN_RUNS_DIR=str(paths.runs_root()))
    result = subprocess.run(
        [
            sys.executable, "-m", "tracker.emit", "phase.start",
            "--emitter", "hook:on-tool-use", "--data", "{not json",
        ],
        cwd=paths.codegen_root(), env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0

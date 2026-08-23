"""PostToolUse hook — the deterministic floor.

Harness-executed, so it **cannot be forgotten** the way a skill's emit instruction can
(vision §5b). It sees every tool call with real timestamps, and knows nothing about
what they meant; the skills supply that meaning. Reconciling the two is how skill
compliance gets measured (architecture §10.4).

Three hard rules, all of which exist because this runs inside the session it observes:

* **exit 0, always** — a non-zero hook can disturb that session;
* **never write to stdout** — the harness may interpret it;
* **never record a raw command string** — a command line can carry an API key
  (architecture §8). Tool name, argv[0], exit code and duration only.
"""

from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tracker import emit  # noqa: E402

EMITTER = "hook:on-tool-use"

#: Tools worth recording. Everything else is noise for this purpose.
INTERESTING = {"Bash", "Write", "Edit", "NotebookEdit"}


def summarise_bash(command: str) -> dict[str, Any]:
    """Reduce a command line to its shape. The text itself never leaves this function."""
    try:
        parts = shlex.split(command)
    except ValueError:
        parts = command.split()
    program = Path(parts[0]).name if parts else ""
    subcommand = parts[1] if len(parts) > 1 and not parts[1].startswith("-") else ""
    return {
        "program": program,
        "subcommand": subcommand if program in {"git", "gh", "python3", "python"} else "",
        "argv_len": len(parts),
        "pytest": _runs_pytest(parts),
    }


def _runs_pytest(parts: list[str]) -> bool:
    """Whether this command runs pytest anywhere, not just as argv[0].

    argv[0] alone misses most real invocations -- `cd x && pytest`, `.venv/bin/pytest`,
    `python -m pytest` -- and the reconciliation pass (architecture §10.4) uses this to
    tell a validated issue from an unvalidated one. Undercounting there quietly weakens
    the only check on whether a skill recorded its validation at all.

    Matches token *basenames* and the `-m pytest` form. It records a single bool, never
    any part of the command line, so the §8 rule that a raw command must not be stored
    still holds.
    """
    for index, token in enumerate(parts):
        if Path(token).name in {"pytest", "py.test"}:
            return True
        if token == "-m" and index + 1 < len(parts) and parts[index + 1] == "pytest":
            return True
    return False


def build_data(payload: dict[str, Any]) -> dict[str, Any] | None:
    """Extract the recordable shape of one tool call, or ``None`` to skip it."""
    tool = str(payload.get("tool_name") or payload.get("tool") or "")
    if tool not in INTERESTING:
        return None

    tool_input = payload.get("tool_input") or {}
    response = payload.get("tool_response") or {}
    data: dict[str, Any] = {"tool": tool}

    if tool == "Bash":
        data.update(summarise_bash(str(tool_input.get("command", ""))))
    else:
        # File path only — never the content, which can contain anything.
        path = str(tool_input.get("file_path", ""))
        data["target"] = Path(path).name if path else ""

    for key in ("exit_code", "duration_ms"):
        if key in response:
            data[key] = response[key]
    if response.get("is_error") or response.get("error"):
        data["errored"] = True
    return data


def main() -> int:
    try:
        raw = sys.stdin.read()
        payload = json.loads(raw) if raw.strip() else {}
        if not isinstance(payload, dict):
            return 0
        data = build_data(payload)
        if data is None:
            return 0
        status = "fail" if data.get("errored") or data.get("exit_code") else "ok"
        emit.emit("tool.used", emitter=EMITTER, scope=_scope(), status=status, data=data)
    except BaseException:  # noqa: BLE001 - a hook must never disturb its session
        pass
    return 0


def _scope() -> dict[str, str]:
    """Best-effort attribution from the run's own state; empty when unknown.

    Deliberately best-effort: guessing wrong is better than recording nothing, and the
    reconciliation pass (architecture §10.4) is what catches a wrong guess.
    """
    try:
        from tracker import paths

        run_id = emit.current_run_id()
        if not run_id:
            return {}
        state_file = paths.state_path(run_id)
        if not state_file.is_file():
            return {}
        with state_file.open(encoding="utf-8") as fh:
            state = json.load(fh)
        return _deepest_open(state.get("tree") or [], {})
    except BaseException:  # noqa: BLE001
        return {}


def _deepest_open(nodes: list[dict[str, Any]], scope: dict[str, str]) -> dict[str, str]:
    """Follow the running branch down to the deepest open node."""
    for node in nodes:
        if node.get("status") != "running":
            continue
        kind = str(node.get("kind", ""))
        if kind in {"phase", "version", "step", "issue"}:
            scope = {**scope, kind: str(node.get("id", ""))}
        return _deepest_open(node.get("children") or [], scope)
    return scope


if __name__ == "__main__":
    sys.exit(main())

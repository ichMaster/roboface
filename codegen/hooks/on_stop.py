"""Stop hook — records nothing about the run's lifecycle, deliberately.

It used to write ``run.aborted`` so a run killed mid-flight would not be left with
``*.start`` and no terminator. That was wrong about when this hook fires: **Stop runs
at the end of every assistant turn**, not when the session ends. A ``/ship-phase`` run
spans many turns by construction, so the first turn boundary closed the run and every
later event was appended to a log that already claimed to be finished — the dashboard
showed *aborted* while the run was still shipping versions.

Worse, it destroyed the signal it meant to protect. An unfinished run is detected by
the **absence** of a terminal event (``tracker.run.pending``), which is what the
orchestrator's resume-or-supersede prompt keys on. Writing a terminal event on every
turn meant a genuinely dead run looked deliberately aborted, so nothing offered to
resume it.

So a stopped session is now recognised the way it always should have been: by a run
with no terminator and no recent events. This module keeps :func:`run_is_open` because
that question is still worth asking — it just no longer answers it by force.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tracker import paths  # noqa: E402

EMITTER = "hook:on-stop"
TERMINAL = {"run.end", "run.aborted"}


def run_is_open(run_id: str) -> bool:
    """True when the run has events but no terminal one."""
    path = paths.events_path(run_id)
    if not path.is_file():
        return False
    seen = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        # Any content at all means a run was underway. A torn line is *evidence* the
        # run died mid-write, so it must count, not be skipped into "nothing ever
        # happened".
        seen = True
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("type") in TERMINAL:
            return False
    return seen


def main() -> int:
    """Do nothing. Kept registered so the reasoning above stays discoverable."""
    return 0


if __name__ == "__main__":
    sys.exit(main())

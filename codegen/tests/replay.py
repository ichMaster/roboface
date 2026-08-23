"""Replay a recorded or generated log into a live run, at an adjustable rate.

TRK-024's second half. Without it the dashboard could only be developed against a
static snapshot -- and its whole contract is about *motion*: debounced re-renders,
held previous frames, no layout jump (dashboard-specification §6.3). Those cannot be
exercised by a file that never changes.

    python3 -m tests.replay clean-run --speed 10
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tests import gen_log  # noqa: E402
from tracker import emit, paths  # noqa: E402


def replay(lines: list[str], run_id: str, speed: float = 10.0, step: float = 0.25) -> int:
    """Append lines one at a time. Returns how many landed."""
    paths.run_dir(run_id).mkdir(parents=True, exist_ok=True)
    paths.current_pointer().parent.mkdir(parents=True, exist_ok=True)
    paths.current_pointer().write_text(run_id, encoding="utf-8")
    target = paths.events_path(run_id)
    target.write_text("", encoding="utf-8")

    delay = step / max(speed, 0.001)
    for line in lines:
        emit.append_line(target, (line + "\n").encode("utf-8"))
        time.sleep(delay)
    return len(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="tests.replay")
    parser.add_argument("source", help="a preset name, or a path to a .jsonl file")
    parser.add_argument("--speed", type=float, default=10.0)
    parser.add_argument("--run-id", default=gen_log.RUN_ID)
    args = parser.parse_args(argv)

    path = Path(args.source)
    text = path.read_text(encoding="utf-8") if path.is_file() else gen_log.preset(args.source)
    count = replay(text.splitlines(), args.run_id, args.speed)
    print(f"replayed {count} events into {args.run_id} at {args.speed}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

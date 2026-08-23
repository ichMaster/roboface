"""Golden frame generator — the device's equivalent of ``gen_log.py``.

Frames are generated from **presets, never from ``runs/``**. That directory is
gitignored, so a golden frame built from a recorded run would be unreproducible in a
fresh checkout and the test would either fail or skip itself into uselessness. The
generator is deterministic — same preset, same bytes — which is what lets the expected
output be committed.

Regeneration is explicit::

    python3 -m tests.gen_frames --update-golden

Never automatic. A golden file that rewrites itself when the code changes is a file
that asserts nothing.
"""

from __future__ import annotations

import json
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from bridge import devices, project
from bridge.devices import Profile
from tests import gen_log
from tracker import reduce as reduce_mod

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "frames"

#: Fixed clock, so an open node's elapsed is the same every run.
NOW = datetime(2026, 8, 3, 18, 0, 0, tzinfo=UTC)

#: The preset every golden frame is built from.
PRESET = "clean-run"

#: A fixed notification payload. One silent event and one alert, so the golden frame
#: exercises both ends of the volume scale and the optional ``g``.
NOTIFICATIONS: list[dict[str, Any]] = [
    {"k": "release", "t": "v01.02 tagged", "b": 0},
    {"k": "retry", "t": "RF-007 x2", "b": 2, "g": devices.SCREEN_FRICTION},
]


def state(preset: str = PRESET) -> dict[str, Any]:
    return reduce_mod.reduce(gen_log.preset(preset).splitlines(), NOW).as_dict()


def targets() -> list[tuple[Profile, int]]:
    """Every (board, screen) pair that exists.

    Nine, not fourteen: the StickC renders NOW and the notification channel only, so
    there is no burndown frame for it to have.
    """
    return [
        (profile, want)
        for profile in (devices.CORE2, devices.STICKC)
        for want in (devices.WANT_NOTIFY, *profile.screens)
    ]


def path_for(profile: Profile, want: int) -> Path:
    return FIXTURES / f"{profile.name}-{devices.WANT_NAMES[want]}.json"


def build(profile: Profile, want: int) -> dict[str, Any]:
    return project.project(
        state(), profile, want, idle_s=0.0, notifications=NOTIFICATIONS
    )


def write_all() -> int:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    for profile, want in targets():
        path_for(profile, want).write_text(
            json.dumps(build(profile, want), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return len(targets())


def main(argv: list[str]) -> int:
    if "--update-golden" not in argv:
        print("refusing to overwrite goldens without --update-golden", file=sys.stderr)
        return 2
    print(f"wrote {write_all()} golden frames to {FIXTURES}")
    return 0


if __name__ == "__main__":  # pragma: no cover - developer entry point
    raise SystemExit(main(sys.argv[1:]))

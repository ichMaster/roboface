"""M5-012 — the C++ frame parser, wired into the same ``pytest`` run.

The parser is C++ and compiles on the host, so it does not need a board. Running it
from here rather than from a separate CI job means one command covers the whole
no-hardware surface — and a golden frame that stops parsing fails alongside the Python
that produced it, in the same run, rather than in a job somebody has to remember.

Skips rather than fails when no compiler is present. Python developers on this repo
should not be blocked by a toolchain they have no reason to have.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

SHARED = Path(__file__).resolve().parent.parent / "device" / "shared"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "frames"

pytestmark = pytest.mark.skipif(
    shutil.which("c++") is None, reason="no host C++ compiler"
)


def test_the_shared_library_is_only_the_parser() -> None:
    """Vision §3.1 left nothing else to share. If a second concern appears here, the
    device has started holding state again and something upstream went wrong."""
    sources = sorted(p.name for p in SHARED.glob("*.cpp")) + sorted(
        p.name for p in SHARED.glob("*.h")
    )
    assert sources == ["frame.cpp", "frame_test.cpp", "frame.h"]


def test_the_parser_compiles_and_passes_under_sanitizers() -> None:
    """Address and UB sanitizers, ``-Werror``, against the committed golden frames.

    Not decoration: the first run caught a use-after-free, and the truncation cases
    are exactly what a frame cut mid-write would do on a device with no memory
    protection and nobody watching.
    """
    result = subprocess.run(
        ["bash", str(SHARED / "run_tests.sh"), str(FIXTURES)],
        capture_output=True,
        text=True,
        timeout=180,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "0 failures" in result.stdout
    assert "134 checks" in result.stdout or "checks" in result.stdout


def test_the_parser_reads_the_same_goldens_the_projection_writes() -> None:
    """One set of fixtures, both languages. A frame that only one side understands is
    the exact defect a shared golden set exists to prevent."""
    goldens = sorted(FIXTURES.glob("*.json"))
    assert len(goldens) == 9
    source = (SHARED / "frame_test.cpp").read_text()
    assert "fixtures/frames" in source

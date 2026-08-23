"""Filesystem locations for the tracking system.

Every path the tracker touches is resolved here, and every one of them lives under
``codegen/`` (vision §3 principle 6). Nothing is hardcoded at the call site: tests
redirect the runs root via ``CODEGEN_RUNS_DIR``, and a hardcoded path would make
that redirection — and therefore test isolation — impossible.

Stdlib only. This module is imported by the emitter, which runs on the pipeline's
critical path.
"""

from __future__ import annotations

import os
from pathlib import Path

#: Overrides the runs root. Set by the test suite to a ``tmp_path``; unset in normal use.
RUNS_DIR_ENV = "CODEGEN_RUNS_DIR"

#: Name of the file holding the active run id, inside the runs root.
CURRENT_NAME = "current"


def codegen_root() -> Path:
    """The ``codegen/`` directory, derived from this file's own location.

    Deriving it from ``__file__`` rather than the working directory means hooks and
    skills can invoke the tracker from anywhere in the repo.
    """
    return Path(__file__).resolve().parent.parent


def runs_root() -> Path:
    """Directory holding one subdirectory per run.

    ``CODEGEN_RUNS_DIR`` wins when set, so tests never touch the real directory.
    """
    override = os.environ.get(RUNS_DIR_ENV)
    if override:
        return Path(override).resolve()
    return codegen_root() / "runs"


def var_root() -> Path:
    """Directory for runtime scratch — logs, PID files, emitter error records.

    Follows the runs root when it is redirected, so a test never writes here either.
    """
    override = os.environ.get(RUNS_DIR_ENV)
    if override:
        return Path(override).resolve() / "var"
    return codegen_root() / "var"


def current_pointer() -> Path:
    """File naming the active run id."""
    return runs_root() / CURRENT_NAME


def run_dir(run_id: str) -> Path:
    """Directory for one run's artefacts."""
    return runs_root() / run_id


def events_path(run_id: str) -> Path:
    """The append-only event log for one run."""
    return run_dir(run_id) / "events.jsonl"


def state_path(run_id: str) -> Path:
    """The derived state snapshot for one run. Disposable; rebuildable from the log."""
    return run_dir(run_id) / "state.json"

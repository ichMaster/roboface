"""Test isolation for the tracking suite.

The emitter resolves the active run from ``codegen/runs/current``. A test that
forgot to redirect that would append its fixtures to — or repoint — **a real run in
progress**. So isolation here is a correctness requirement, not hygiene
(architecture §10):

* an autouse fixture points ``CODEGEN_RUNS_DIR`` at a per-test ``tmp_path``;
* a guard fails any test whose resolved runs root is the real directory, so the
  protection cannot be silently lost by someone unsetting the variable.
"""

from __future__ import annotations

import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

# The suite runs without installing anything, so put `codegen/` on the path and import
# `tracker` as a TOP-LEVEL package. Importing it as `codegen.tracker` instead would make
# mypy see each file under two module names, and would make `codegen/` a package
# candidate for the generated project's flat-layout build discovery.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tracker import paths  # noqa: E402

# reset.py sits at codegen/ root, not inside a package.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

#: The directory that must never be written to by a test.
REAL_RUNS_ROOT = paths.codegen_root() / "runs"


@pytest.fixture(autouse=True)
def isolated_runs_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Path]:
    """Point every path helper at a throwaway directory, then verify it took effect.

    Autouse, so a test cannot opt out by forgetting. The post-condition is asserted
    rather than assumed: if ``runs_root()`` ever stops honouring the environment
    variable, every test fails loudly instead of quietly writing to the real log.
    """
    runs = tmp_path / "runs"
    runs.mkdir()
    monkeypatch.setenv(paths.RUNS_DIR_ENV, str(runs))

    resolved = paths.runs_root()
    if resolved == REAL_RUNS_ROOT.resolve():
        pytest.fail(
            "Test isolation failed: runs_root() resolved to the real "
            f"{REAL_RUNS_ROOT} despite {paths.RUNS_DIR_ENV} being set. "
            "A test must never write to a live run."
        )
    yield runs


@pytest.fixture
def unisolated_runs_dir(monkeypatch: pytest.MonkeyPatch) -> Iterator[None]:
    """Deliberately remove the redirection — only for testing the guard itself."""
    monkeypatch.delenv(paths.RUNS_DIR_ENV, raising=False)
    yield

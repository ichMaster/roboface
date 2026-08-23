"""TRK-001 — path resolution and the test-isolation guard."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from tracker import paths


def test_runs_root_honours_the_env_override(isolated_runs_dir: Path) -> None:
    assert paths.runs_root() == isolated_runs_dir.resolve()


def test_runs_root_defaults_to_codegen_runs(unisolated_runs_dir: None) -> None:
    """Without the override, the runs root is codegen/runs -- never anywhere else."""
    assert paths.runs_root() == paths.codegen_root() / "runs"


def test_every_path_stays_under_the_runs_root(isolated_runs_dir: Path) -> None:
    """Containment: no helper may resolve outside the redirected root."""
    root = isolated_runs_dir.resolve()
    for candidate in (
        paths.current_pointer(),
        paths.run_dir("run-20260803-142012"),
        paths.events_path("run-20260803-142012"),
        paths.state_path("run-20260803-142012"),
        paths.var_root(),
    ):
        assert root in candidate.parents or candidate == root, candidate


def test_codegen_root_is_derived_from_the_module_not_the_cwd(tmp_path: Path) -> None:
    """Hooks and skills invoke the tracker from anywhere, so cwd must not matter."""
    script = (
        f"import sys; sys.path.insert(0, {str(paths.codegen_root())!r})\n"
        "from tracker import paths\n"
        "print(paths.codegen_root())\n"
    )
    out = subprocess.run(
        [sys.executable, "-c", script],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=True,
    )
    assert out.stdout.strip() == str(paths.codegen_root())


def test_the_isolation_guard_fires_when_redirection_is_lost(
    pytester: pytest.Pytester,
) -> None:
    """The guard must fail a mis-pointed test rather than let it write to the real log.

    Run as a nested pytest session so the failure is observed, not raised into this one.
    """
    codegen_dir = paths.codegen_root()
    pytester.makeconftest(
        f"""
        import sys, pytest
        sys.path.insert(0, {str(codegen_dir)!r})
        from tracker import paths

        REAL = paths.codegen_root() / "runs"

        @pytest.fixture(autouse=True)
        def isolated_runs_dir(monkeypatch):
            # Deliberately broken: does not redirect, mimicking a lost override.
            monkeypatch.delenv(paths.RUNS_DIR_ENV, raising=False)
            if paths.runs_root() == REAL.resolve():
                pytest.fail("Test isolation failed: runs_root() resolved to the real directory.")
            yield
        """
    )
    pytester.makepyfile("def test_writes_somewhere(): assert True")
    result = pytester.runpytest()
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*Test isolation failed*"])


def test_testpaths_resolves_so_the_app_suite_is_never_swept_in() -> None:
    """The tracker suite must collect only itself.

    testpaths is relative to rootdir, which is codegen/ whether pytest walks up from
    inside it or is pointed here with `-c codegen/pyproject.toml`. It read
    "codegen/tests" once, resolving to codegen/codegen/tests -- which does not exist,
    so pytest fell back to collecting recursively from the cwd. That looked correct
    only while the generated application had no tests/ directory of its own; the
    moment a run created one, both suites were silently merged into one number.
    """
    import tomllib

    codegen = paths.codegen_root()
    with (codegen / "pyproject.toml").open("rb") as handle:
        config = tomllib.load(handle)

    testpaths = config["tool"]["pytest"]["ini_options"]["testpaths"]
    assert testpaths, "testpaths must be set, or collection falls back to the cwd"
    for entry in testpaths:
        resolved = codegen / entry
        assert resolved.is_dir(), (
            f"testpaths entry {entry!r} resolves to {resolved}, which does not exist; "
            "pytest would silently collect recursively from the cwd instead"
        )

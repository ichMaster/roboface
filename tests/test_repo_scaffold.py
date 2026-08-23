"""The scaffold's own guarantees.

Two suites live in this repository and must never merge: the product's (`tests/`) and the
generation tracker's (`codegen/tests/`). `codegen/pyproject.toml` documents the failure
mode from the other side -- when `testpaths` does not resolve, pytest falls back to
collecting recursively from the working directory and quietly sweeps the other suite in.
That is invisible in a green run and only shows up as a wrong test count, so it is
asserted here rather than trusted.
"""

from __future__ import annotations

import subprocess
import sys
import tomllib
from pathlib import Path


def _pyproject(repo_root: Path) -> dict[str, object]:
    with (repo_root / "pyproject.toml").open("rb") as handle:
        return tomllib.load(handle)


def test_pytest_is_configured_for_the_product_suite_only(repo_root: Path) -> None:
    config = _pyproject(repo_root)
    pytest_config = config["tool"]["pytest"]["ini_options"]  # type: ignore[call-overload, index]

    assert pytest_config["testpaths"] == ["tests"]
    # "tests" is on the path so conftest.py can import the shared fake-device harness;
    # the tests tree is deliberately __init__.py-free.
    assert pytest_config["pythonpath"] == ["server", "tests"]


def test_root_collection_never_reaches_codegen(repo_root: Path) -> None:
    """Collect from the repo root and assert not one `codegen/` test comes back."""
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "--collect-only", "-q"],
        cwd=repo_root,
        capture_output=True,
        text=True,
        timeout=300,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    collected = [line for line in result.stdout.splitlines() if "::" in line]
    assert collected, "collection returned no tests at all -- testpaths is probably wrong"
    assert not any(line.startswith("codegen/") for line in collected), (
        "codegen/ tests were swept into the product suite"
    )


def test_codegen_keeps_its_own_configuration(repo_root: Path) -> None:
    # The counterpart of the rule above: codegen/ is tested from inside codegen/, with its
    # own pyproject. If that file ever disappears, the two suites merge silently.
    assert (repo_root / "codegen" / "pyproject.toml").is_file()


def test_server_env_is_not_tracked(repo_root: Path) -> None:
    """`server/.env` holds every provider key and must never enter the repository."""
    result = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "server/.env"],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )

    assert result.returncode != 0, "server/.env is tracked by git -- secrets would be committed"
    assert (repo_root / "server" / ".env.example").is_file(), "the committed template is missing"


def test_env_example_documents_the_v0_1_variables(repo_root: Path) -> None:
    # ARCHITECTURE §Configuration marks these three as v0.1; the template is the committed
    # shape of server/.env and must carry them.
    text = (repo_root / "server" / ".env.example").read_text(encoding="utf-8")

    for key in ("ROBOFACE_WS_HOST", "ROBOFACE_WS_PORT", "ROBOFACE_LOG_LEVEL"):
        assert f"{key}=" in text, f"{key} is missing from server/.env.example"


def test_a_stalled_test_fails_rather_than_hanging(repo_root: Path) -> None:
    """A suite-level deadline exists (code review #4).

    The fake device blocks on a queue with no deadline of its own, so this is the only thing
    standing between a regression that stops the server replying and a CI job that hangs
    until its wall-clock limit. Asserted rather than assumed, because a missing plugin or a
    dropped setting fails silently -- the suite still passes, it just stops being bounded.
    """
    config = _pyproject(repo_root)
    pytest_config = config["tool"]["pytest"]["ini_options"]  # type: ignore[call-overload, index]

    assert isinstance(pytest_config["timeout"], int)
    assert 0 < pytest_config["timeout"] <= 300

    requirements = (repo_root / "requirements-dev.txt").read_text(encoding="utf-8")
    assert "pytest-timeout" in requirements, "the setting is inert without the plugin"

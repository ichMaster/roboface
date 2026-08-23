"""TRK-001 — the stdlib-only guarantee for code on the pipeline's critical path.

``tracker/`` and ``hooks/`` run inside a ``/ship-phase`` run. An emitter that cannot
import is an emitter that can break a build, which is exactly what the never-raise
guarantee forbids (architecture §5.2). So they may import nothing that needs
installing — and that is enforced here rather than trusted, because the failure is
silent until the day someone runs the pipeline on a machine without the extra
package.

``dashboard/`` is exempt: it is a separate process, started deliberately, and is
allowed its own dependencies from ``codegen/requirements.txt``.
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

import pytest

CODEGEN = Path(__file__).resolve().parent.parent

#: Packages within the tracking system itself, importable without installation.
LOCAL_ROOTS = {"tracker", "hooks"}

#: Directories whose code runs on the pipeline's critical path.
CRITICAL_PATH = ("tracker", "hooks")


def _imported_roots(path: Path) -> set[str]:
    """Top-level module names imported by one file."""
    roots: set[str] = set()
    for node in ast.walk(ast.parse(path.read_text(), filename=str(path))):
        if isinstance(node, ast.Import):
            roots.update(alias.name.split(".")[0] for alias in node.names)
        # level > 0 is a relative import: local by definition.
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            roots.add(node.module.split(".")[0])
    return roots


def _critical_path_modules() -> list[Path]:
    return sorted(
        p
        for directory in CRITICAL_PATH
        for p in (CODEGEN / directory).rglob("*.py")
        if "__pycache__" not in p.parts
    )


def test_there_is_code_to_check() -> None:
    """Guard against the suite passing vacuously once files move or are renamed."""
    assert _critical_path_modules(), "no modules found under tracker/ or hooks/"


@pytest.mark.parametrize("module", _critical_path_modules(), ids=lambda p: p.name)
def test_critical_path_imports_only_stdlib(module: Path) -> None:
    third_party = {
        root
        for root in _imported_roots(module)
        if root not in sys.stdlib_module_names and root not in LOCAL_ROOTS
    }
    assert not third_party, (
        f"{module.relative_to(CODEGEN)} imports {sorted(third_party)}, which would need "
        "installing. Code on the pipeline's critical path must be stdlib-only."
    )


def test_requirements_are_documented_as_dashboard_and_test_only() -> None:
    """The file must say what it is for; a bare list invites the wrong import."""
    text = (CODEGEN / "requirements.txt").read_text()
    assert "stdlib-only" in text
    assert "tracker" in text and "hooks" in text

"""Shared fixtures for the RoboFace server suite.

Kept deliberately small: v0.1 needs only paths. The fake-device fixtures that every later
phase drives the WS protocol with arrive in RF-004 and live here alongside these.
"""

from __future__ import annotations

from pathlib import Path

import pytest

#: The repository root -- `tests/` sits directly beneath it.
REPO_ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture(scope="session")
def repo_root() -> Path:
    """The repository root, for tests that reason about layout or run tooling."""
    return REPO_ROOT

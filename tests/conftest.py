"""Shared fixtures for the RoboFace server suite.

The fake-device fixtures live here because every phase from v1 on drives the protocol
through them: audio in v1, images in v3. Keeping one harness is what stops the suite from
growing several divergent opinions about the wire.
"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import pytest
from fake_device import FakeDevice, connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.router import ConnectionRegistry

#: The repository root -- `tests/` sits directly beneath it.
REPO_ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture(scope="session")
def repo_root() -> Path:
    """The repository root, for tests that reason about layout or run tooling."""
    return REPO_ROOT


@pytest.fixture
def registry() -> ConnectionRegistry:
    """A registry the test can inspect after the connection is gone."""
    return ConnectionRegistry()


@pytest.fixture
def app(registry: ConnectionRegistry) -> FastAPI:
    """An application with the default (echo) responder and an observable registry.

    Built per test: two tests must never be able to see each other's connections.
    """
    return create_app(registry=registry)


@pytest.fixture
def device(app: FastAPI) -> Iterator[FakeDevice]:
    """A connected fake device. Not yet greeted -- `hello` is the test's to send."""
    with connect(app) as fake:
        yield fake

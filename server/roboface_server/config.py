"""Server configuration -- the variables this phase actually reads, and no others.

Values come from the process environment, falling back to ``server/.env`` and then to
the defaults below. A missing ``.env`` is not an error: a fresh checkout and the test
suite both run on defaults, and only a *deployment* needs the file.

ARCHITECTURE.md §Configuration and secrets assigns every variable to the phase that
introduces it. This module deliberately reads only the three marked **v0.1** --
``ROBOFACE_WS_HOST``, ``ROBOFACE_WS_PORT``, ``ROBOFACE_LOG_LEVEL``. Reading a key ahead
of the phase that owns it would let an unset future variable look configured; each later
phase adds its own here when it lands.

Secrets are never read here and never logged: the provider keys arrive with their
provider seams from v0.2 onward, and they live in ``server/.env`` only.
"""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from dotenv import dotenv_values

#: ``server/.env`` -- this file is ``server/roboface_server/config.py``, so the server
#: directory is one level up. Gitignored; ``server/.env.example`` is its committed shape.
DEFAULT_ENV_FILE = Path(__file__).resolve().parent.parent / ".env"

# All interfaces by design: the device reaches the server across the LAN, so binding
# loopback would make the product unusable on the hardware it is written for.
DEFAULT_WS_HOST = "0.0.0.0"
DEFAULT_WS_PORT = 8000
DEFAULT_LOG_LEVEL = "info"

#: The accepted values of ``ROBOFACE_LOG_LEVEL``. Lower-case on the wire and in the
#: file; the logging module maps them upward when it configures itself (RF-005).
LOG_LEVELS = frozenset({"debug", "info", "warning", "error", "critical"})

#: Port 8000 is the product's, and only the product's. ``codegen/``'s dashboard owns
#: 8420 precisely so the two can run side by side during a generation run.
_MIN_PORT = 1
_MAX_PORT = 65535


class ConfigError(ValueError):
    """A configuration value is present but unusable.

    Raised rather than defaulted: silently falling back to 8000 because someone typed
    ``ROBOFACE_WS_PORT=800O`` would hide the problem until the device could not connect.
    """


@dataclass(frozen=True, slots=True)
class Settings:
    """The resolved v0.1 configuration. Frozen -- read once at startup, never mutated."""

    ws_host: str
    ws_port: int
    log_level: str


def _read_env_file(env_file: Path) -> dict[str, str]:
    """The ``.env`` file as a plain mapping. A missing file is an empty mapping."""
    if not env_file.is_file():
        return {}
    # dotenv_values yields ``str | None`` (a bare ``KEY`` line has no value); a valueless
    # key is treated as absent rather than as an empty string, so it falls to the default.
    return {key: value for key, value in dotenv_values(env_file).items() if value is not None}


def _resolve(
    key: str,
    environ: Mapping[str, str],
    file_values: Mapping[str, str],
) -> str | None:
    """Process environment first, then the file, then absent.

    That precedence is the conventional one and the operationally useful one: a value
    exported for one run overrides the checked-in deployment file without editing it.
    An empty string counts as absent -- ``ROBOFACE_WS_HOST=`` in a template means
    "unset", not "bind to the empty host".
    """
    for source in (environ, file_values):
        raw = source.get(key)
        if raw is not None and raw.strip():
            return raw.strip()
    return None


def _parse_port(raw: str) -> int:
    try:
        port = int(raw)
    except ValueError as exc:
        raise ConfigError(f"ROBOFACE_WS_PORT must be an integer, got {raw!r}") from exc
    if not _MIN_PORT <= port <= _MAX_PORT:
        raise ConfigError(f"ROBOFACE_WS_PORT must be in {_MIN_PORT}..{_MAX_PORT}, got {port}")
    return port


def _parse_log_level(raw: str) -> str:
    level = raw.lower()
    if level not in LOG_LEVELS:
        allowed = ", ".join(sorted(LOG_LEVELS))
        raise ConfigError(f"ROBOFACE_LOG_LEVEL must be one of {allowed}, got {raw!r}")
    return level


def load_settings(
    environ: Mapping[str, str] | None = None,
    *,
    env_file: Path | None = None,
) -> Settings:
    """Resolve the v0.1 settings.

    Both sources are injectable so the tests never touch the real process environment or
    the developer's real ``server/.env`` -- a suite that depends on either is a suite that
    passes on one machine.
    """
    resolved_environ = os.environ if environ is None else environ
    resolved_file = DEFAULT_ENV_FILE if env_file is None else env_file
    file_values = _read_env_file(resolved_file)

    host = _resolve("ROBOFACE_WS_HOST", resolved_environ, file_values) or DEFAULT_WS_HOST
    raw_port = _resolve("ROBOFACE_WS_PORT", resolved_environ, file_values)
    raw_level = _resolve("ROBOFACE_LOG_LEVEL", resolved_environ, file_values)

    return Settings(
        ws_host=host,
        ws_port=DEFAULT_WS_PORT if raw_port is None else _parse_port(raw_port),
        log_level=DEFAULT_LOG_LEVEL if raw_level is None else _parse_log_level(raw_level),
    )

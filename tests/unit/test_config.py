"""Unit tests for `roboface_server.config`.

Every case injects both the environment and the `.env` path, so no test reads the
developer's real `server/.env` or mutates `os.environ`.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from roboface_server.config import (
    DEFAULT_LOG_LEVEL,
    DEFAULT_WS_HOST,
    DEFAULT_WS_PORT,
    ConfigError,
    Settings,
    load_settings,
)


def _no_file(tmp_path: Path) -> Path:
    """A path inside tmp_path that deliberately does not exist."""
    return tmp_path / "absent.env"


def test_defaults_apply_when_nothing_is_set(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))

    assert settings == Settings(
        ws_host=DEFAULT_WS_HOST,
        ws_port=DEFAULT_WS_PORT,
        log_level=DEFAULT_LOG_LEVEL,
    )


def test_missing_env_file_is_not_an_error(tmp_path: Path) -> None:
    absent = _no_file(tmp_path)
    assert not absent.exists()

    # The assertion is simply that this returns rather than raising: a fresh checkout
    # has no server/.env, and the suite must run there.
    assert load_settings({}, env_file=absent).ws_port == DEFAULT_WS_PORT


def test_env_file_values_are_read(tmp_path: Path) -> None:
    env_file = tmp_path / ".env"
    env_file.write_text(
        "ROBOFACE_WS_HOST=127.0.0.1\nROBOFACE_WS_PORT=9001\nROBOFACE_LOG_LEVEL=debug\n",
        encoding="utf-8",
    )

    settings = load_settings({}, env_file=env_file)

    assert settings == Settings(ws_host="127.0.0.1", ws_port=9001, log_level="debug")


def test_process_environment_overrides_the_file(tmp_path: Path) -> None:
    env_file = tmp_path / ".env"
    env_file.write_text("ROBOFACE_WS_PORT=9001\n", encoding="utf-8")

    settings = load_settings({"ROBOFACE_WS_PORT": "9999"}, env_file=env_file)

    assert settings.ws_port == 9999


def test_blank_value_falls_through_to_the_default(tmp_path: Path) -> None:
    # `KEY=` in a template means "unset", not "the empty string" -- server/.env.example
    # ships exactly that shape for every secret.
    env_file = tmp_path / ".env"
    env_file.write_text("ROBOFACE_WS_HOST=\n", encoding="utf-8")

    assert load_settings({"ROBOFACE_WS_HOST": "  "}, env_file=env_file).ws_host == DEFAULT_WS_HOST


@pytest.mark.parametrize("raw", ["800O", "8000.5", "-1", "0", "70000"])
def test_unusable_port_raises_rather_than_defaulting(raw: str, tmp_path: Path) -> None:
    # A typo must not silently become 8000 -- the failure would surface much later, as a
    # device that cannot connect. (An *empty* value is "absent" by design and is covered
    # by test_blank_value_falls_through_to_the_default.)
    with pytest.raises(ConfigError):
        load_settings({"ROBOFACE_WS_PORT": raw}, env_file=_no_file(tmp_path))


def test_log_level_is_case_insensitive(tmp_path: Path) -> None:
    settings = load_settings({"ROBOFACE_LOG_LEVEL": "DEBUG"}, env_file=_no_file(tmp_path))

    assert settings.log_level == "debug"


def test_unknown_log_level_raises(tmp_path: Path) -> None:
    with pytest.raises(ConfigError):
        load_settings({"ROBOFACE_LOG_LEVEL": "verbose"}, env_file=_no_file(tmp_path))


def test_settings_are_frozen(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))

    with pytest.raises(AttributeError):
        settings.ws_port = 1234  # type: ignore[misc]


def test_only_the_v0_1_variables_are_read(tmp_path: Path) -> None:
    """A key belonging to a later phase must not leak into v0.1's settings.

    ARCHITECTURE §Configuration assigns each variable to a phase; reading one early would
    let an unset future key look configured.
    """
    settings = load_settings(
        {"GEMINI_API_KEY": "should-never-be-read", "ROBOFACE_CANON_PATH": "canon.md"},
        env_file=_no_file(tmp_path),
    )

    values = [str(getattr(settings, field)) for field in settings.__dataclass_fields__]
    assert not any("should-never-be-read" in value for value in values)
    assert set(settings.__dataclass_fields__) == {"ws_host", "ws_port", "log_level"}

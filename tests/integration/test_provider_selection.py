"""How the application chooses its responder, and what that must never cost.

The rule this file exists to hold: **the suite runs with no API key and reaches no network.**
Everything else here is in service of that — an injected responder always wins, a bare
`create_app()` never reaches for configuration, and the real provider is built only where a
conversation is genuinely about to start.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from fake_device import connect
from fastapi import FastAPI
from roboface_server.app import build_responder, create_app
from roboface_server.config import ConfigError, Settings, load_settings
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import TextIn
from roboface_server.providers import MockLLMProvider
from roboface_server.router import EchoResponder

#: Deliberately a path that does not exist. `load_settings()` with no `env_file` reads the
#: developer's real `server/.env`, which would make these tests depend on what happens to be
#: configured on the machine running them — and would put a real API key in their reach.
_NO_ENV_FILE = Path(__file__).resolve().parent / "does-not-exist.env"


def _settings(**overrides: object) -> Settings:
    base = load_settings({}, env_file=_NO_ENV_FILE)
    fields = {field: getattr(base, field) for field in base.__dataclass_fields__}
    fields.update(overrides)
    return Settings(**fields)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------------------
# The injected responder always wins
# ---------------------------------------------------------------------------------------


def test_an_injected_responder_is_used_verbatim() -> None:
    orchestrator = Orchestrator(provider=MockLLMProvider(deltas=["ok"]))
    app = create_app(responder=orchestrator)

    assert app.state.router.responder is orchestrator


def test_an_injected_responder_wins_over_settings() -> None:
    """Even with settings supplied, injection wins — otherwise a test could pay for a turn."""
    orchestrator = Orchestrator(provider=MockLLMProvider(deltas=["ok"]))

    app = create_app(
        responder=orchestrator,
        settings=_settings(gemini_api_key="a-key-that-must-not-be-used"),
    )

    assert app.state.router.responder is orchestrator


def test_the_existing_suite_still_works_through_the_new_default() -> None:
    app = create_app(responder=Orchestrator(provider=MockLLMProvider(deltas=["при", "віт"])))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="hi"))
        reply = device.collect_reply()

    assert reply.text == "привіт"
    assert len(reply) == 2


# ---------------------------------------------------------------------------------------
# A bare create_app() costs nothing
# ---------------------------------------------------------------------------------------


def test_a_bare_create_app_does_not_reach_for_configuration() -> None:
    """It falls back to the echo rather than to Gemini.

    Reaching for the environment here would make an accidental `create_app()` — in a test, a
    script, a REPL — construct a real client against whatever key happened to be exported.
    """
    app = create_app()

    assert isinstance(app.state.router.responder, EchoResponder)


def test_a_bare_create_app_still_serves_a_turn(monkeypatch: pytest.MonkeyPatch) -> None:
    # Even with a key in the environment: the echo is chosen because nothing asked for more.
    monkeypatch.setenv("GEMINI_API_KEY", "a-key-that-must-not-be-used")
    app: FastAPI = create_app()

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))

        assert device.collect_reply().text == "привіт"


# ---------------------------------------------------------------------------------------
# Selecting the real provider
# ---------------------------------------------------------------------------------------


def test_selecting_the_real_provider_without_a_key_fails_at_startup() -> None:
    """Not on the first turn.

    The same outage either way, but discovered before a device is connected and with a
    message that names the variable and the file it belongs in.
    """
    with pytest.raises(ConfigError, match="GEMINI_API_KEY"):
        build_responder(_settings(gemini_api_key=""))


def test_the_failure_message_points_at_the_file_to_edit() -> None:
    with pytest.raises(ConfigError) as raised:
        build_responder(_settings(gemini_api_key=""))

    assert "server/.env" in str(raised.value)


def test_the_key_is_not_in_the_failure_message() -> None:
    # It is absent in this case by definition — but the assertion guards the day someone
    # "improves" the message by interpolating what was found.
    with pytest.raises(ConfigError) as raised:
        build_responder(_settings(gemini_api_key=""))

    assert "AIza" not in str(raised.value)


def test_building_the_real_responder_produces_an_orchestrator_over_gemini() -> None:
    from roboface_server.providers.gemini import GeminiProvider

    responder = build_responder(_settings(gemini_api_key="AIza-not-real", gemini_model="m"))

    assert isinstance(responder, Orchestrator)
    assert isinstance(responder.provider, GeminiProvider)
    assert responder.provider.model == "m"
    assert responder.provider.thinking_budget == 0


def test_the_configured_thinking_budget_reaches_the_provider() -> None:
    responder = build_responder(
        _settings(gemini_api_key="AIza-not-real", gemini_thinking_budget=128)
    )

    assert isinstance(responder, Orchestrator)
    assert responder.provider.thinking_budget == 128  # type: ignore[union-attr]


# ---------------------------------------------------------------------------------------
# The rule
# ---------------------------------------------------------------------------------------


def test_no_test_in_this_suite_needs_an_api_key(monkeypatch: pytest.MonkeyPatch) -> None:
    """With the environment stripped bare, the app still builds and serves a turn."""
    for variable in ("GEMINI_API_KEY", "GEMINI_MODEL", "GEMINI_THINKING_BUDGET"):
        monkeypatch.delenv(variable, raising=False)

    app = create_app(responder=Orchestrator(provider=MockLLMProvider(deltas=["ok"])))

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="hi"))

        assert device.collect_reply().text == "ok"

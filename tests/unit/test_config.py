"""Unit tests for `roboface_server.config`.

Every case injects both the environment and the `.env` path, so no test reads the
developer's real `server/.env` or mutates `os.environ`.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from roboface_server.config import (
    DEFAULT_DEEPGRAM_ENCODING,
    DEFAULT_DEEPGRAM_ENDPOINT_MS,
    DEFAULT_DEEPGRAM_LANGUAGE,
    DEFAULT_DEEPGRAM_MODEL,
    DEFAULT_DEEPGRAM_SAMPLE_RATE,
    DEFAULT_ELEVENLABS_MODEL,
    DEFAULT_ELEVENLABS_OUTPUT_FORMAT,
    DEFAULT_ELEVENLABS_VOICE_SETTINGS,
    DEFAULT_GEMINI_MODEL,
    DEFAULT_GEMINI_THINKING_BUDGET,
    DEFAULT_LOG_LEVEL,
    DEFAULT_WS_HOST,
    DEFAULT_WS_PORT,
    ConfigError,
    Settings,
    load_settings,
)


def _settings(**overrides: object) -> Settings:
    """A Settings with every field defaulted, so a test states only what it is about."""
    fields: dict[str, object] = {
        "ws_host": DEFAULT_WS_HOST,
        "ws_port": DEFAULT_WS_PORT,
        "log_level": DEFAULT_LOG_LEVEL,
        "gemini_api_key": "",
        "gemini_model": DEFAULT_GEMINI_MODEL,
        "gemini_thinking_budget": DEFAULT_GEMINI_THINKING_BUDGET,
        "elevenlabs_api_key": "",
        "elevenlabs_voice_id": "",
        "elevenlabs_model": DEFAULT_ELEVENLABS_MODEL,
        "elevenlabs_output_format": DEFAULT_ELEVENLABS_OUTPUT_FORMAT,
        "elevenlabs_voice_settings": dict(DEFAULT_ELEVENLABS_VOICE_SETTINGS),
        "deepgram_api_key": "",
        "deepgram_model": DEFAULT_DEEPGRAM_MODEL,
        "deepgram_language": DEFAULT_DEEPGRAM_LANGUAGE,
        "deepgram_encoding": DEFAULT_DEEPGRAM_ENCODING,
        "deepgram_sample_rate": DEFAULT_DEEPGRAM_SAMPLE_RATE,
        "deepgram_endpoint_ms": DEFAULT_DEEPGRAM_ENDPOINT_MS,
    }
    fields.update(overrides)
    return Settings(**fields)  # type: ignore[arg-type]


def _no_file(tmp_path: Path) -> Path:
    """A path inside tmp_path that deliberately does not exist."""
    return tmp_path / "absent.env"


def test_defaults_apply_when_nothing_is_set(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))

    assert settings == _settings()


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

    assert settings == _settings(ws_host="127.0.0.1", ws_port=9001, log_level="debug")


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


def test_only_the_landed_phases_variables_are_read(tmp_path: Path) -> None:
    """A key belonging to a *later* phase must not leak into settings.

    ARCHITECTURE §Configuration assigns each variable to the phase that introduces it, and
    reading one early would let an unset future key look configured. v0.2 added the three
    Gemini keys, v1.1 the four ElevenLabs ones (five since the voice gained its settings) and
    **v1.3 the six Deepgram ones**, so all of those are now expected; the canon key is not, and
    arrives with v4.1.

    This test is meant to fail when a phase lands, and it did: it was asserting that
    ``ELEVENLABS_API_KEY`` must not be read, which was true right up until RF-030. Updating it
    is the deliberate act of moving a variable from "later" to "now".
    """
    settings = load_settings(
        {
            "ROBOFACE_CANON_PATH": "should-never-be-read",
        },
        env_file=_no_file(tmp_path),
    )

    values = [str(getattr(settings, field)) for field in settings.__dataclass_fields__]
    assert not any("should-never-be-read" in value for value in values)
    assert set(settings.__dataclass_fields__) == {
        "ws_host",
        "ws_port",
        "log_level",
        "gemini_api_key",
        "gemini_model",
        "gemini_thinking_budget",
        "elevenlabs_api_key",
        "elevenlabs_voice_id",
        "elevenlabs_model",
        "elevenlabs_output_format",
        "elevenlabs_voice_settings",
        "deepgram_api_key",
        "deepgram_model",
        "deepgram_language",
        "deepgram_encoding",
        "deepgram_sample_rate",
        "deepgram_endpoint_ms",
    }


# ---------------------------------------------------------------------------------------
# The Gemini settings (v0.2)
# ---------------------------------------------------------------------------------------


def test_gemini_defaults_apply(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))

    assert settings.gemini_model == DEFAULT_GEMINI_MODEL == "gemini-2.5-flash"
    # 0 is the documented reason this model was chosen: lowest time-to-first-token.
    assert settings.gemini_thinking_budget == 0
    assert settings.gemini_api_key == ""


def test_gemini_settings_are_overridable(tmp_path: Path) -> None:
    settings = load_settings(
        {
            "GEMINI_API_KEY": "a-key",
            "GEMINI_MODEL": "gemini-2.5-flash-lite",
            "GEMINI_THINKING_BUDGET": "512",
        },
        env_file=_no_file(tmp_path),
    )

    assert settings.gemini_api_key == "a-key"
    assert settings.gemini_model == "gemini-2.5-flash-lite"
    assert settings.gemini_thinking_budget == 512


def test_loading_settings_works_with_no_api_key(tmp_path: Path) -> None:
    """The regression that matters: the whole suite runs on a machine with no key."""
    settings = load_settings({}, env_file=_no_file(tmp_path))

    assert settings.gemini_api_key == ""


def test_requiring_the_key_fails_clearly_when_it_is_absent(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))

    with pytest.raises(ConfigError, match="GEMINI_API_KEY"):
        settings.require_gemini_api_key()


def test_requiring_the_key_returns_it_when_present(tmp_path: Path) -> None:
    settings = load_settings({"GEMINI_API_KEY": "a-key"}, env_file=_no_file(tmp_path))

    assert settings.require_gemini_api_key() == "a-key"


@pytest.mark.parametrize("raw", ["lots", "-1", "0.5"])
def test_an_unusable_thinking_budget_raises(raw: str, tmp_path: Path) -> None:
    with pytest.raises(ConfigError, match="GEMINI_THINKING_BUDGET"):
        load_settings({"GEMINI_THINKING_BUDGET": raw}, env_file=_no_file(tmp_path))


def test_inline_comments_in_the_env_file_are_not_part_of_the_value(tmp_path: Path) -> None:
    # server/.env.example annotates every key with a trailing comment, and server/.env is a
    # copy of it. A model id carrying "# chat + the vision turn" would reach the API verbatim.
    env_file = tmp_path / ".env"
    env_file.write_text(
        "GEMINI_MODEL=gemini-2.5-flash          # chat + the vision turn (v3.1)\n",
        encoding="utf-8",
    )

    assert load_settings({}, env_file=env_file).gemini_model == "gemini-2.5-flash"


# ---------------------------------------------------------------------------------------
# The key never renders (code review #1)
# ---------------------------------------------------------------------------------------


SECRET = "AIzaSyTOTALLY-SECRET-KEY-12345"


def test_repr_never_contains_the_api_key(tmp_path: Path) -> None:
    """A Settings reaches text by too many ordinary routes to rely on nobody taking one.

    The generated dataclass repr printed all 39 characters of a live credential.
    """
    settings = load_settings({"GEMINI_API_KEY": SECRET}, env_file=_no_file(tmp_path))

    assert SECRET not in repr(settings)
    assert SECRET not in str(settings)
    assert SECRET not in f"{settings}"


def test_repr_shows_whether_a_key_is_present_and_nothing_more(tmp_path: Path) -> None:
    # Not a prefix, not a length: both are still information about a credential, and neither
    # helps anyone debug more than the boolean does.
    with_key = repr(load_settings({"GEMINI_API_KEY": SECRET}, env_file=_no_file(tmp_path)))
    without = repr(load_settings({}, env_file=_no_file(tmp_path)))

    assert "gemini_api_key=***" in with_key
    assert "gemini_api_key=unset" in without
    assert SECRET[:6] not in with_key
    assert str(len(SECRET)) not in with_key


TTS_SECRET = "sk_TOTALLY-SECRET-ELEVENLABS-KEY-99"


def test_repr_never_contains_the_tts_key_either(tmp_path: Path) -> None:
    """v1.1 added a second vendor, and a redaction covering only the first is one that fails
    the moment the system grows -- which is exactly how the original finding happened."""
    settings = load_settings(
        {"GEMINI_API_KEY": SECRET, "ELEVENLABS_API_KEY": TTS_SECRET},
        env_file=_no_file(tmp_path),
    )

    rendered = repr(settings)
    assert SECRET not in rendered
    assert TTS_SECRET not in rendered
    assert "elevenlabs_api_key=***" in rendered


def test_the_tts_key_absence_is_shown_the_same_way(tmp_path: Path) -> None:
    assert "elevenlabs_api_key=unset" in repr(load_settings({}, env_file=_no_file(tmp_path)))


ASR_SECRET = "dg_TOTALLY-SECRET-DEEPGRAM-KEY-77"


def test_repr_never_contains_the_asr_key_either(tmp_path: Path) -> None:
    """Three vendors now. A redaction that covered two would fail on the third, which is how the
    original finding happened in the first place."""
    settings = load_settings(
        {"GEMINI_API_KEY": SECRET, "ELEVENLABS_API_KEY": TTS_SECRET,
         "DEEPGRAM_API_KEY": ASR_SECRET},
        env_file=_no_file(tmp_path),
    )
    rendered = repr(settings)
    for secret in (SECRET, TTS_SECRET, ASR_SECRET):
        assert secret not in rendered
    assert "deepgram_api_key=***" in rendered


def test_the_non_secret_tts_settings_are_still_visible(tmp_path: Path) -> None:
    # The voice id and model are configuration, not credentials: redacting them would move the
    # debugging problem without protecting anything.
    settings = load_settings(
        {"ELEVENLABS_VOICE_ID": "voice-xyz", "ELEVENLABS_MODEL": "eleven_turbo_v2_5"},
        env_file=_no_file(tmp_path),
    )
    rendered = repr(settings)
    assert "voice-xyz" in rendered
    assert "eleven_turbo_v2_5" in rendered


def test_repr_still_shows_the_non_secret_settings(tmp_path: Path) -> None:
    # A redacting repr that redacted everything would just move the debugging problem.
    settings = load_settings(
        {"GEMINI_API_KEY": SECRET, "ROBOFACE_WS_PORT": "9001", "GEMINI_MODEL": "m"},
        env_file=_no_file(tmp_path),
    )

    rendered = repr(settings)
    assert "9001" in rendered
    assert "'m'" in rendered
    assert "log_level='info'" in rendered


def test_the_key_does_not_leak_through_a_structured_log_line(tmp_path: Path) -> None:
    """The concrete route that worried the review: JsonFormatter stringifies with default=str."""
    import json as json_module
    from io import StringIO

    from roboface_server.logging import configure, connection_context, log

    stream = StringIO()
    configure("debug", stream=stream)
    settings = load_settings({"GEMINI_API_KEY": SECRET}, env_file=_no_file(tmp_path))

    with connection_context("s"):
        log("startup", settings=settings)

    rendered = stream.getvalue()
    assert SECRET not in rendered
    assert json_module.loads(rendered)["settings"].startswith("Settings(")


# ---------------------------------------------------------------------------------------
# The voice's delivery (v1.4)
# ---------------------------------------------------------------------------------------


def test_voice_settings_default_to_the_documented_character(tmp_path: Path) -> None:
    settings = load_settings({}, env_file=_no_file(tmp_path))
    assert settings.elevenlabs_voice_settings == DEFAULT_ELEVENLABS_VOICE_SETTINGS


def test_voice_settings_override_only_what_they_name(tmp_path: Path) -> None:
    # A partial object is a partial override, not a replacement: naming `stability` must not
    # silently drop `similarity_boost` and take the voice's own default for it instead.
    settings = load_settings(
        {"ELEVENLABS_VOICE_SETTINGS": '{"stability": 0.7}'}, env_file=_no_file(tmp_path)
    )
    assert settings.elevenlabs_voice_settings["stability"] == 0.7
    assert (
        settings.elevenlabs_voice_settings["similarity_boost"]
        == DEFAULT_ELEVENLABS_VOICE_SETTINGS["similarity_boost"]
    )


def test_malformed_voice_settings_are_refused_not_ignored(tmp_path: Path) -> None:
    # The whole point of validating these. A voice that is subtly wrong is far harder to notice
    # than one that fails to load: nothing errors, replies still play, and the character is simply
    # not the one the specification describes.
    for bad in ('{"stability": 0.4', '["stability"]', '{"stabilty": 0.4}', '{"stability": "high"}'):
        with pytest.raises(ConfigError):
            load_settings({"ELEVENLABS_VOICE_SETTINGS": bad}, env_file=_no_file(tmp_path))

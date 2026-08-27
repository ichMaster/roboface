"""Server configuration -- the variables this phase actually reads, and no others.

Values come from the process environment, falling back to ``server/.env`` and then to
the defaults below. A missing ``.env`` is not an error: a fresh checkout and the test
suite both run on defaults, and only a *deployment* needs the file.

ARCHITECTURE.md §Configuration and secrets assigns every variable to the phase that
introduces it, and this module reads **only** the ones whose phase has landed -- the three
marked v0.1 (``ROBOFACE_WS_*``, ``ROBOFACE_LOG_LEVEL``) and, from v0.2, the three Gemini keys.
Reading a key ahead of the phase that owns it would let an unset future variable look
configured; each later phase adds its own here when it lands.

``GEMINI_API_KEY`` is read but **never logged and never carried into an error message**. It is
also not required to *load* settings -- only to select the real provider. That split is the
point: the test suite must run with no key present, while a server started to actually talk to
someone must fail at startup rather than on its first turn.
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

#: Gemini defaults, per ARCHITECTURE §Configuration and secrets. The model is the only chat
#: vendor this project has (MISSION §Principles); ``thinking_budget`` is 0 because
#: time-to-first-token is the reason this model was chosen at all.
DEFAULT_GEMINI_MODEL = "gemini-2.5-flash"
DEFAULT_GEMINI_THINKING_BUDGET = 0

#: ElevenLabs defaults, from v1.1. ``pcm_16000`` is not a preference -- it is the device's playback
#: format (``AUDIO_FMT`` in ``protocol.py``), so asking for it means nothing decodes anywhere: what
#: the vendor streams is what goes on the wire and what the speaker plays. Asking for MP3 would put
#: a decoder on an ESP32 in the middle of a latency budget.
DEFAULT_ELEVENLABS_MODEL = "eleven_turbo_v2_5"
DEFAULT_ELEVENLABS_OUTPUT_FORMAT = "pcm_16000"

#: Deepgram defaults, from v1.3. `linear16` at 16 kHz is the device's capture format unchanged --
#: the same `AUDIO_FMT` that TTS returns, so audio crosses the whole system without a conversion.
DEFAULT_DEEPGRAM_MODEL = "nova-2"
DEFAULT_DEEPGRAM_LANGUAGE = "uk"
DEFAULT_DEEPGRAM_ENCODING = "linear16"
DEFAULT_DEEPGRAM_SAMPLE_RATE = 16000
DEFAULT_DEEPGRAM_ENDPOINT_MS = 500

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
    """The resolved configuration. Frozen -- read once at startup, never mutated."""

    ws_host: str
    ws_port: int
    log_level: str
    gemini_api_key: str
    gemini_model: str
    gemini_thinking_budget: int
    elevenlabs_api_key: str
    elevenlabs_voice_id: str
    elevenlabs_model: str
    elevenlabs_output_format: str
    deepgram_api_key: str
    deepgram_model: str
    deepgram_language: str
    deepgram_encoding: str
    deepgram_sample_rate: int
    deepgram_endpoint_ms: int

    def __repr__(self) -> str:
        """Never render the key.

        The generated dataclass ``__repr__`` printed all 39 characters of a live credential,
        and a ``Settings`` reaches text by too many ordinary routes to rely on nobody taking
        one: an unhandled traceback that renders locals, a ``log(..., settings=settings)``
        that the JSON formatter would happily stringify, a ``print`` while debugging, a crash
        reporter. The v0.1 review's standard for a log line -- that it stays safe to paste
        into an issue -- applies at least as strongly to the module that owns the secrets.

        ``***`` and ``unset``, never a prefix and never a length: both of those are still
        information about a credential, and neither helps anyone debug more than knowing
        whether it is there at all.
        """
        key = "***" if self.gemini_api_key else "unset"
        # Every key gets the same treatment. v1.1 added a second vendor, and a redaction that
        # covered only the first would be a redaction that fails the moment the system grows --
        # which is exactly how the original finding happened.
        tts_key = "***" if self.elevenlabs_api_key else "unset"
        asr_key = "***" if self.deepgram_api_key else "unset"
        return (
            f"Settings(ws_host={self.ws_host!r}, ws_port={self.ws_port!r}, "
            f"log_level={self.log_level!r}, gemini_api_key={key}, "
            f"gemini_model={self.gemini_model!r}, "
            f"gemini_thinking_budget={self.gemini_thinking_budget!r}, "
            f"elevenlabs_api_key={tts_key}, "
            f"elevenlabs_voice_id={self.elevenlabs_voice_id!r}, "
            f"elevenlabs_model={self.elevenlabs_model!r}, "
            f"elevenlabs_output_format={self.elevenlabs_output_format!r}, "
            f"deepgram_api_key={asr_key}, "
            f"deepgram_model={self.deepgram_model!r}, "
            f"deepgram_language={self.deepgram_language!r})"
        )

    def require_gemini_api_key(self) -> str:
        """The key, or a clear failure.

        Called where the **real** provider is selected, not at load time. A suite must run
        with no key present; a server about to hold a conversation must not start without one
        and then fail on its first turn, which is the same outage discovered later and with
        less information attached.
        """
        if not self.gemini_api_key:
            raise ConfigError(
                "GEMINI_API_KEY is not set. Put it in server/.env "
                "(see server/.env.example), or inject a responder for tests."
            )
        return self.gemini_api_key


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


def _parse_thinking_budget(raw: str) -> int:
    try:
        budget = int(raw)
    except ValueError as exc:
        raise ConfigError(f"GEMINI_THINKING_BUDGET must be an integer, got {raw!r}") from exc
    if budget < 0:
        raise ConfigError(f"GEMINI_THINKING_BUDGET cannot be negative, got {budget}")
    return budget


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

    api_key = _resolve("GEMINI_API_KEY", resolved_environ, file_values)
    model = _resolve("GEMINI_MODEL", resolved_environ, file_values)
    raw_budget = _resolve("GEMINI_THINKING_BUDGET", resolved_environ, file_values)

    tts_key = _resolve("ELEVENLABS_API_KEY", resolved_environ, file_values)
    voice_id = _resolve("ELEVENLABS_VOICE_ID", resolved_environ, file_values)
    tts_model = _resolve("ELEVENLABS_MODEL", resolved_environ, file_values)
    tts_format = _resolve("ELEVENLABS_OUTPUT_FORMAT", resolved_environ, file_values)

    asr_key = _resolve("DEEPGRAM_API_KEY", resolved_environ, file_values)
    asr_model = _resolve("DEEPGRAM_MODEL", resolved_environ, file_values)
    asr_language = _resolve("DEEPGRAM_LANGUAGE", resolved_environ, file_values)
    asr_encoding = _resolve("DEEPGRAM_ENCODING", resolved_environ, file_values)
    asr_rate = _resolve("DEEPGRAM_SAMPLE_RATE", resolved_environ, file_values)
    asr_endpoint = _resolve("DEEPGRAM_ENDPOINT_MS", resolved_environ, file_values)

    return Settings(
        ws_host=host,
        ws_port=DEFAULT_WS_PORT if raw_port is None else _parse_port(raw_port),
        log_level=DEFAULT_LOG_LEVEL if raw_level is None else _parse_log_level(raw_level),
        # Absent, not an error: loading settings must work with no key, so the suite does.
        gemini_api_key=api_key or "",
        gemini_model=model or DEFAULT_GEMINI_MODEL,
        gemini_thinking_budget=(
            DEFAULT_GEMINI_THINKING_BUDGET
            if raw_budget is None
            else _parse_thinking_budget(raw_budget)
        ),
        # Absent is not an error here either: the suite must load settings with no keys at all,
        # and a missing TTS key becomes an enumerated failure at the moment speech is attempted.
        elevenlabs_api_key=tts_key or "",
        elevenlabs_voice_id=voice_id or "",
        elevenlabs_model=tts_model or DEFAULT_ELEVENLABS_MODEL,
        elevenlabs_output_format=tts_format or DEFAULT_ELEVENLABS_OUTPUT_FORMAT,
        deepgram_api_key=asr_key or "",
        deepgram_model=asr_model or DEFAULT_DEEPGRAM_MODEL,
        deepgram_language=asr_language or DEFAULT_DEEPGRAM_LANGUAGE,
        deepgram_encoding=asr_encoding or DEFAULT_DEEPGRAM_ENCODING,
        deepgram_sample_rate=(
            DEFAULT_DEEPGRAM_SAMPLE_RATE if asr_rate is None else int(asr_rate)
        ),
        deepgram_endpoint_ms=(
            DEFAULT_DEEPGRAM_ENDPOINT_MS if asr_endpoint is None else int(asr_endpoint)
        ),
    )

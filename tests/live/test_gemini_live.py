"""The one test that spends money. Skipped unless it is explicitly asked for.

ARCHITECTURE §Testing and CI: *"CI never makes a paid call; a live call is opt-in and
manual."* This file is that opt-in, and the gate is deliberately awkward: an env var set to a
specific word, not merely the presence of an API key. A key is present on the developer's
machine and in `server/.env` all the time — gating on it would make "run the suite" a paid
operation for whoever happened to have one configured.

Run it deliberately:

    ROBOFACE_LIVE_TESTS=1 .venv/bin/pytest tests/live -v
"""

from __future__ import annotations

import os

import pytest
from roboface_server.prompt import build_system_prompt
from roboface_server.providers import Message

#: The gate. A value of exactly "1" — not "any truthy string" — so an unrelated variable
#: someone exports cannot switch paid calls on by accident.
LIVE_GATE = "ROBOFACE_LIVE_TESTS"

live = pytest.mark.skipif(
    os.environ.get(LIVE_GATE) != "1",
    reason=f"live Gemini call: set {LIVE_GATE}=1 to opt in (this spends money)",
)


@live
@pytest.mark.asyncio
async def test_a_real_gemini_turn_streams_deltas() -> None:
    """The v0.2 DoD's manual half: a real answer, arriving as a stream.

    Asserts the shape rather than the words — the model's actual text is not ours to pin, but
    "more than one delta, and the first is not the whole thing" is.
    """
    from roboface_server.providers.gemini import GeminiProvider

    # Read the environment directly rather than through `config`: this is a smoke test of
    # the *adapter*, and keeping it independent of configuration means it neither waits for
    # RF-011 nor breaks when configuration is refactored. RF-011's own manual DoD check is
    # what exercises the wiring.
    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        pytest.skip("GEMINI_API_KEY is not set")

    provider = GeminiProvider(
        api_key,
        model=os.environ.get("GEMINI_MODEL", "gemini-2.5-flash"),
        thinking_budget=int(os.environ.get("GEMINI_THINKING_BUDGET", "0")),
    )

    deltas: list[str] = []
    stream = provider.stream(
        build_system_prompt(),
        [Message(role="user", text="Привіт! Скажи щось коротке про себе.")],
    )
    async for delta in stream:
        deltas.append(delta)

    assert deltas, "the model returned nothing at all"
    joined = "".join(deltas)
    assert joined.strip()
    assert len(deltas) > 1, "the reply arrived in one piece — check the stream endpoint"
    assert deltas[0] != joined, "the first delta was the whole reply"


@live
@pytest.mark.asyncio
async def test_a_bad_key_is_translated_not_raised_as_a_vendor_type() -> None:
    from roboface_server.providers.base import ProviderError
    from roboface_server.providers.gemini import GeminiProvider

    provider = GeminiProvider("AIza-definitely-not-a-valid-key")

    with pytest.raises(ProviderError):
        [delta async for delta in provider.stream("s", [Message(role="user", text="hi")])]

"""Unit tests for the streaming turn.

Two properties carry this module and both are easy to lose silently: that deltas leave as they
arrive rather than at the end, and that the first-token budget distinguishes a provider which
has not started from one that is merely slow between tokens. A single timeout around the whole
stream would pass a naive test and truncate real replies.
"""

from __future__ import annotations

import asyncio

import pytest
from roboface_server.orchestrator import (
    DEFAULT_FIRST_TOKEN_BUDGET_S,
    Orchestrator,
    TurnAborted,
)
from roboface_server.protocol import ErrorCode
from roboface_server.providers import Message, MockLLMProvider, ProviderError, SilentLLMProvider


def _orchestrator(provider: object, **kwargs: object) -> Orchestrator:
    return Orchestrator(provider=provider, **kwargs)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------------------
# Streaming
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_turn_yields_the_provider_deltas_in_order() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["При", "віт", "!"]))

    deltas = [delta async for delta in orchestrator.respond("s", "hi")]

    assert deltas == ["При", "віт", "!"]


@pytest.mark.asyncio
async def test_the_first_delta_is_available_before_the_stream_ends() -> None:
    # The property, stated directly: a caller can act on delta 0 while the provider still has
    # work to do. An implementation that collected the reply would fail here and nowhere else.
    provider = MockLLMProvider(deltas=["a", "b", "c"], delay_s=0.2, delay_before_index=2)
    orchestrator = _orchestrator(provider)

    stream = orchestrator.respond("s", "hi")
    started = asyncio.get_running_loop().time()
    first = await anext(stream)
    elapsed = asyncio.get_running_loop().time() - started

    assert first == "a"
    assert elapsed < 0.2, "the first delta waited for the rest of the stream"


@pytest.mark.asyncio
async def test_respond_is_not_a_coroutine_function() -> None:
    # The caller must get the iterator immediately, so it can await the *first* delta under
    # its own deadline rather than awaiting the whole turn.
    import inspect

    assert not inspect.iscoroutinefunction(_orchestrator(MockLLMProvider()).respond)


@pytest.mark.asyncio
async def test_the_provider_receives_the_system_prompt_and_the_history() -> None:
    provider = MockLLMProvider()
    orchestrator = _orchestrator(provider, system_prompt="SYSTEM")

    [delta async for delta in orchestrator.respond("s", "привіт")]

    system, messages = provider.calls[0]
    assert system == "SYSTEM"
    assert messages == (Message(role="user", text="привіт"),)


# ---------------------------------------------------------------------------------------
# The first-token budget -- both directions
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_budget_fires_when_the_provider_never_starts() -> None:
    provider = MockLLMProvider(delay_s=5.0, delay_before_index=0)
    orchestrator = _orchestrator(provider, first_token_budget_s=0.05)

    with pytest.raises(TurnAborted) as raised:
        [delta async for delta in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.LLM_TIMEOUT


@pytest.mark.asyncio
async def test_the_budget_does_not_fire_on_a_slow_middle() -> None:
    """The distinction the budget exists to make.

    A model mid-sentence is working. Cutting it off would truncate a healthy reply at exactly
    the moment it was going well -- which is what a single timeout around the whole stream
    would do, and why this test exists as the counterpart to the one above.
    """
    provider = MockLLMProvider(deltas=["a", "b", "c"], delay_s=0.15, delay_before_index=1)
    orchestrator = _orchestrator(provider, first_token_budget_s=0.05)

    deltas = [delta async for delta in orchestrator.respond("s", "hi")]

    assert deltas == ["a", "b", "c"]


def test_the_default_budget_matches_the_architecture() -> None:
    # ARCHITECTURE §Budgets and abort semantics puts the LLM first-token budget at ~8 s.
    assert DEFAULT_FIRST_TOKEN_BUDGET_S == 8.0


# ---------------------------------------------------------------------------------------
# History
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_history_accumulates_user_and_model_messages() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ві", "таю"]))

    [delta async for delta in orchestrator.respond("s", "привіт")]

    assert orchestrator.history("s") == (
        Message(role="user", text="привіт"),
        Message(role="model", text="вітаю"),
    )


@pytest.mark.asyncio
async def test_the_model_message_is_the_joined_deltas() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["one ", "two ", "three"]))

    [delta async for delta in orchestrator.respond("s", "count")]

    assert orchestrator.history("s")[-1].text == "one two three"


@pytest.mark.asyncio
async def test_a_second_turn_sees_the_first() -> None:
    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)

    [delta async for delta in orchestrator.respond("s", "first")]
    [delta async for delta in orchestrator.respond("s", "second")]

    _, messages = provider.calls[1]
    assert [message.text for message in messages] == ["first", "ok", "second"]


@pytest.mark.asyncio
async def test_two_sessions_do_not_share_a_history() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))

    [delta async for delta in orchestrator.respond("session-a", "from a")]
    [delta async for delta in orchestrator.respond("session-b", "from b")]

    assert [m.text for m in orchestrator.history("session-a")] == ["from a", "ok"]
    assert [m.text for m in orchestrator.history("session-b")] == ["from b", "ok"]


@pytest.mark.asyncio
async def test_forget_drops_a_session() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [delta async for delta in orchestrator.respond("s", "hi")]

    orchestrator.forget("s")

    assert orchestrator.history("s") == ()


@pytest.mark.asyncio
async def test_history_is_returned_as_a_copy() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [delta async for delta in orchestrator.respond("s", "hi")]

    snapshot = orchestrator.history("s")
    [delta async for delta in orchestrator.respond("s", "again")]

    assert len(snapshot) == 2, "a caller's snapshot changed underneath it"


# ---------------------------------------------------------------------------------------
# Failure translation (completed in RF-009)
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_an_unhinted_provider_failure_becomes_llm_failed() -> None:
    provider = MockLLMProvider(fail_at_index=0, error=ProviderError("no idea"))
    orchestrator = _orchestrator(provider)

    with pytest.raises(TurnAborted) as raised:
        [delta async for delta in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.LLM_FAILED


@pytest.mark.asyncio
async def test_a_hinted_provider_failure_keeps_its_code() -> None:
    error = ProviderError("quota", ErrorCode.RATE_LIMITED)
    orchestrator = _orchestrator(MockLLMProvider(fail_at_index=0, error=error))

    with pytest.raises(TurnAborted) as raised:
        [delta async for delta in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.RATE_LIMITED


@pytest.mark.asyncio
async def test_a_silent_provider_ends_the_turn_cleanly() -> None:
    # Not a failure: ARCHITECTURE treats silence as a clean end to a turn, and a spurious
    # llm_failed would put an error face on the device for a turn that had nothing to say.
    orchestrator = _orchestrator(SilentLLMProvider())

    deltas = [delta async for delta in orchestrator.respond("s", "hi")]

    assert deltas == []

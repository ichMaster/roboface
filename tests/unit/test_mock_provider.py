"""Unit tests for the mock provider's three levers.

RF-008 and RF-009 test streaming, the first-token budget and mid-stream failure entirely
through this mock. If a lever silently does nothing, those tests pass while asserting
nothing — so the levers are checked here rather than trusted downstream.
"""

from __future__ import annotations

import asyncio
import time

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers import (
    DEFAULT_DELTAS,
    Message,
    MockLLMProvider,
    ProviderError,
    SilentLLMProvider,
)


@pytest.mark.asyncio
async def test_the_mock_is_deterministic() -> None:
    # A mock that varies is a suite that flakes, and a flaky suite is one nobody reads the
    # failures of.
    first = [delta async for delta in MockLLMProvider().stream("s", [])]
    second = [delta async for delta in MockLLMProvider().stream("s", [])]

    assert first == second == list(DEFAULT_DELTAS)


@pytest.mark.asyncio
async def test_custom_deltas_are_yielded_verbatim() -> None:
    deltas = [delta async for delta in MockLLMProvider(deltas=["один", "два"]).stream("s", [])]

    assert deltas == ["один", "два"]


# ---------------------------------------------------------------------------------------
# Lever 1: failure at a chosen index
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_failure_before_the_first_delta() -> None:
    provider = MockLLMProvider(deltas=["a", "b"], fail_at_index=0)

    with pytest.raises(ProviderError):
        [delta async for delta in provider.stream("s", [])]


@pytest.mark.asyncio
async def test_failure_part_way_through_yields_what_came_before() -> None:
    # The mid-stream case RF-009 needs: some deltas arrived, then the provider died. The
    # device must get an error rather than a reply{final: true} that makes half a sentence
    # look finished.
    provider = MockLLMProvider(deltas=["a", "b", "c"], fail_at_index=2)
    seen: list[str] = []

    with pytest.raises(ProviderError):
        async for delta in provider.stream("s", []):
            seen.append(delta)

    assert seen == ["a", "b"]


@pytest.mark.asyncio
async def test_failure_after_the_last_delta() -> None:
    # A stream that ends in an error rather than a completion -- a different case from dying
    # part of the way through, and one worth being able to express.
    provider = MockLLMProvider(deltas=["a"], fail_at_index=5)
    seen: list[str] = []

    with pytest.raises(ProviderError):
        async for delta in provider.stream("s", []):
            seen.append(delta)

    assert seen == ["a"]


@pytest.mark.asyncio
async def test_the_injected_error_is_the_one_raised() -> None:
    error = ProviderError("out of quota", ErrorCode.RATE_LIMITED)
    provider = MockLLMProvider(fail_at_index=0, error=error)

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in provider.stream("s", [])]

    assert raised.value is error
    assert raised.value.code is ErrorCode.RATE_LIMITED


# ---------------------------------------------------------------------------------------
# Lever 2: a delay, positioned
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_delay_before_the_first_delta_stalls_the_stream() -> None:
    provider = MockLLMProvider(deltas=["a", "b"], delay_s=0.05, delay_before_index=0)

    started = time.monotonic()
    stream = provider.stream("s", [])
    await anext(stream)

    assert time.monotonic() - started >= 0.05


@pytest.mark.asyncio
async def test_a_delay_mid_stream_does_not_stall_the_first_delta() -> None:
    # The distinction the first-token budget exists to make: a provider that has not started
    # is broken; one that is merely slow between tokens is not, and cutting it off would
    # truncate a healthy reply mid-sentence.
    provider = MockLLMProvider(deltas=["a", "b"], delay_s=0.05, delay_before_index=1)

    started = time.monotonic()
    stream = provider.stream("s", [])
    await anext(stream)
    first_delta_took = time.monotonic() - started

    assert first_delta_took < 0.05
    await asyncio.wait_for(anext(stream), timeout=5)


# ---------------------------------------------------------------------------------------
# Lever 3: what the provider was actually asked
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_call_is_recorded_for_assertion() -> None:
    provider = MockLLMProvider()
    history = [Message(role="user", text="привіт")]

    [delta async for delta in provider.stream("SYSTEM", history)]

    assert provider.calls == [("SYSTEM", tuple(history))]


# ---------------------------------------------------------------------------------------
# The silent provider
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_silent_provider_yields_nothing_and_does_not_raise() -> None:
    # Not a failure. ARCHITECTURE treats silence as a clean end to a turn, and the model
    # declining to speak is the same shape -- RF-009 must not turn it into llm_failed.
    deltas = [delta async for delta in SilentLLMProvider().stream("s", [])]

    assert deltas == []

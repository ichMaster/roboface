"""The `TTSProvider` seam.

A contract test exists to stop the seam drifting, and for this one there is a specific drift worth
naming: an implementation that gathers a whole phrase before yielding satisfies every type in the
signature and defeats the entire phase. So these assert the *streaming* shape, not only the types.
"""

from __future__ import annotations

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ProviderError, TTSProvider
from roboface_server.providers.mock import DEFAULT_CHUNKS, MockTTSProvider


def test_the_mock_satisfies_the_seam() -> None:
    assert isinstance(MockTTSProvider(), TTSProvider)


def test_synthesize_returns_the_iterator_without_awaiting() -> None:
    # Not `async def`: the caller must hold the iterator before awaiting anything, so it can put a
    # first-audio budget on the first chunk alone. An `async def` would make that impossible.
    provider = MockTTSProvider()
    stream = provider.synthesize("Привіт.")
    assert hasattr(stream, "__anext__")


@pytest.mark.asyncio
async def test_chunks_are_bytes_and_there_is_more_than_one() -> None:
    provider = MockTTSProvider()
    chunks = [chunk async for chunk in provider.synthesize("Привіт.")]
    assert len(chunks) > 1, "a single-chunk mock would let an accumulating consumer pass"
    assert all(isinstance(chunk, bytes) for chunk in chunks)


@pytest.mark.asyncio
async def test_chunks_hold_whole_pcm16_samples() -> None:
    # PCM16 is two bytes per sample. An odd-length chunk is not a smaller chunk, it is a malformed
    # one, and the device would play the rest of the phrase shifted by a byte.
    provider = MockTTSProvider()
    async for chunk in provider.synthesize("Привіт."):
        assert len(chunk) % 2 == 0


@pytest.mark.asyncio
async def test_the_provider_records_the_phrase_it_was_given() -> None:
    provider = MockTTSProvider()
    async for _ in provider.synthesize("Перше речення."):
        pass
    async for _ in provider.synthesize("Друге."):
        pass
    assert provider.calls == ["Перше речення.", "Друге."]


@pytest.mark.asyncio
async def test_a_failure_carries_the_enumerated_code() -> None:
    provider = MockTTSProvider(fail_at_index=0)
    with pytest.raises(ProviderError) as raised:
        async for _ in provider.synthesize("Привіт."):
            pass
    assert raised.value.code is ErrorCode.TTS_FAILED


@pytest.mark.asyncio
async def test_a_failure_mid_phrase_yields_what_came_before_it() -> None:
    # The chunks already emitted were true when they left. A mid-phrase failure is not a reason to
    # pretend the earlier audio never existed -- the device has already played it.
    provider = MockTTSProvider(fail_at_index=2)
    seen: list[bytes] = []
    with pytest.raises(ProviderError):
        async for chunk in provider.synthesize("Привіт."):
            seen.append(chunk)
    assert seen == list(DEFAULT_CHUNKS[:2])

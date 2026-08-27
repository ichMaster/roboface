"""The `ASRProvider` seam.

The drift this guards against is specific: a seam that could only be *called* rather than *fed*
would force recognition to start after the audio was complete, which is the ~1.4 s v1.3 exists to
remove. So these assert that pushing and reading overlap.
"""

from __future__ import annotations

import asyncio

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ASRChunk, ASRProvider, ASRSession, ProviderError
from roboface_server.providers.mock import DEFAULT_ASR_SCRIPT, MockASRProvider


def test_the_mock_satisfies_the_seam() -> None:
    provider = MockASRProvider()
    assert isinstance(provider, ASRProvider)
    assert isinstance(provider.open(), ASRSession)


def test_open_returns_the_session_without_awaiting() -> None:
    # Not `async def`: the caller holds the session before awaiting anything, so it can push the
    # first frame under its own budget.
    session = MockASRProvider().open()
    assert hasattr(session, "push")


@pytest.mark.asyncio
async def test_audio_can_be_pushed_while_results_are_read() -> None:
    # The property the whole seam exists for. If these could not overlap, recognition would have to
    # wait for the audio to finish.
    #
    # `settle_after` is what makes the overlap observable rather than assumed: the recogniser
    # releases its transcript **after two frames, while two more are still being pushed**. A mock
    # that answered before hearing anything would satisfy a weaker version of this test and would
    # model a vendor that cannot exist.
    provider = MockASRProvider(settle_after=2)
    session = provider.open()
    seen: list[ASRChunk] = []
    pushed_when_first_seen: int | None = None

    async def read() -> None:
        nonlocal pushed_when_first_seen
        async for chunk in session.results():
            if pushed_when_first_seen is None:
                pushed_when_first_seen = len(session.pushed)
            seen.append(chunk)

    async def feed() -> None:
        for index in range(4):
            await session.push(bytes([index]) * 640)
            await asyncio.sleep(0)
        await session.finish()

    await asyncio.gather(read(), feed())
    assert len(session.pushed) == 4
    assert seen == list(DEFAULT_ASR_SCRIPT)
    # Read *during* the push sequence, not after it.
    assert pushed_when_first_seen is not None and pushed_when_first_seen < 4


@pytest.mark.asyncio
async def test_interims_are_revisions_not_additions() -> None:
    # The default script revises rather than only growing. A consumer that accumulated interims
    # would produce "прив привіт привіт як" and pass any test whose script only appended.
    interims = [chunk.text for chunk in DEFAULT_ASR_SCRIPT if not chunk.is_final]
    assert len(interims) > 1
    assert interims[-1] != "".join(interims)


@pytest.mark.asyncio
async def test_the_session_records_what_it_was_fed() -> None:
    provider = MockASRProvider()
    session = provider.open()
    await session.push(b"\x00\x01" * 320)
    await session.finish()
    assert session.pushed == [b"\x00\x01" * 320]
    assert session.finished


@pytest.mark.asyncio
async def test_close_is_idempotent() -> None:
    session = MockASRProvider().open()
    await session.close()
    await session.close()
    assert session.closed


@pytest.mark.asyncio
async def test_a_failure_carries_the_enumerated_code() -> None:
    provider = MockASRProvider(error=ProviderError("mock asr failure", ErrorCode.ASR_FAILED))
    session = provider.open()
    with pytest.raises(ProviderError) as raised:
        async for _ in session.results():
            pass
    assert raised.value.code is ErrorCode.ASR_FAILED

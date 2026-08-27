"""The Deepgram adapter, against an injected fake socket.

**No paid call happens here.** The connector is injectable precisely so this file can exist; without
it every test of this adapter would be a network call and none would run in CI.
"""

from __future__ import annotations

import json
from collections.abc import AsyncIterator
from typing import Any
from urllib.parse import parse_qs, urlparse

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ProviderError
from roboface_server.providers.deepgram import (
    DeepgramProvider,
    build_listen_url,
)


class FakeSocket:
    """Replays scripted Deepgram messages and records what was sent to it."""

    def __init__(self, messages: list[str], fail_on_send: bool = False) -> None:
        self._messages = messages
        self.sent: list[Any] = []
        self.closed = False
        self.fail_on_send = fail_on_send

    async def send(self, data: Any) -> None:
        if self.fail_on_send:
            raise ConnectionError("socket is gone")
        self.sent.append(data)

    async def close(self) -> None:
        self.closed = True

    def __aiter__(self) -> AsyncIterator[str]:
        return self._iter()

    async def _iter(self) -> AsyncIterator[str]:
        for message in self._messages:
            yield message


def transcript(text: str, is_final: bool) -> str:
    return json.dumps(
        {"channel": {"alternatives": [{"transcript": text}]}, "is_final": is_final}
    )


def provider(socket: FakeSocket) -> DeepgramProvider:
    async def connector(url: str, headers: dict[str, str]) -> FakeSocket:
        connector.url = url  # type: ignore[attr-defined]
        connector.headers = headers  # type: ignore[attr-defined]
        return socket

    p = DeepgramProvider(
        "key-123",
        model="nova-2",
        language="uk",
        encoding="linear16",
        sample_rate=16000,
        connector=connector,
    )
    p.connector = connector  # type: ignore[attr-defined]
    return p


# --- the URL ---------------------------------------------------------------------------


def test_the_url_carries_every_parameter_the_phase_depends_on() -> None:
    # None of these fail loudly when wrong: omit interim_results and there are simply no interims,
    # which looks like a slow network. This string is the only place they are visible.
    url = build_listen_url(model="nova-2", language="uk", encoding="linear16", sample_rate=16000)
    query = parse_qs(urlparse(url).query)

    assert urlparse(url).scheme == "wss", "batch REST pays its latency after the person stops"
    assert query["model"] == ["nova-2"]
    assert query["language"] == ["uk"]
    assert query["encoding"] == ["linear16"]
    assert query["sample_rate"] == ["16000"]
    assert query["channels"] == ["1"]
    assert query["interim_results"] == ["true"]
    assert query["smart_format"] == ["true"]
    assert query["endpointing"] == ["500"]
    assert query["utterance_end_ms"] == ["1000"]


def test_the_backstop_is_longer_than_the_endpoint() -> None:
    # It is a backstop *for* endpointing. If it fired first it would pre-empt the thing it exists
    # to cover for.
    query = parse_qs(urlparse(build_listen_url(
        model="m", language="uk", encoding="linear16", sample_rate=16000)).query)
    assert int(query["utterance_end_ms"][0]) > int(query["endpointing"][0])


# --- the session -----------------------------------------------------------------------


def test_a_missing_key_is_enumerated_not_a_traceback() -> None:
    with pytest.raises(ProviderError) as raised:
        DeepgramProvider("", model="m", language="uk", encoding="linear16", sample_rate=16000)
    assert raised.value.code is ErrorCode.ASR_FAILED


@pytest.mark.asyncio
async def test_interims_and_finals_come_through_as_chunks() -> None:
    socket = FakeSocket([transcript("прив", False), transcript("Привіт.", True)])
    session = provider(socket).open()
    chunks = [chunk async for chunk in session.results()]

    assert [(c.text, c.is_final) for c in chunks] == [("прив", False), ("Привіт.", True)]


@pytest.mark.asyncio
async def test_audio_pushed_mid_session_reaches_the_socket() -> None:
    socket = FakeSocket([transcript("ок", True)])
    session = provider(socket).open()
    await session.push(b"\x00\x01" * 320)
    await session.push(b"\x02\x03" * 320)
    assert len(socket.sent) == 2


@pytest.mark.asyncio
async def test_utterance_end_becomes_an_empty_final() -> None:
    # The tracker's backstop signal. An empty final releases whatever is held.
    socket = FakeSocket([json.dumps({"type": "UtteranceEnd", "last_word_end": 1.2})])
    session = provider(socket).open()
    chunks = [chunk async for chunk in session.results()]
    assert len(chunks) == 1
    assert chunks[0].is_final and chunks[0].text == ""


@pytest.mark.asyncio
async def test_metadata_and_junk_are_ignored_rather_than_fatal() -> None:
    # Metadata arrives on the same socket. Treating an unrecognised message as an error would make
    # a working vendor look broken -- the same judgement the firmware makes about declared types.
    socket = FakeSocket([
        json.dumps({"type": "Metadata", "request_id": "abc"}),
        "not json at all",
        transcript("Привіт.", True),
    ])
    session = provider(socket).open()
    chunks = [chunk async for chunk in session.results()]
    assert [c.text for c in chunks] == ["Привіт."]


@pytest.mark.asyncio
async def test_an_empty_transcript_is_not_a_chunk() -> None:
    socket = FakeSocket([transcript("   ", False), transcript("Привіт.", True)])
    session = provider(socket).open()
    chunks = [chunk async for chunk in session.results()]
    assert [c.text for c in chunks] == ["Привіт."]


@pytest.mark.asyncio
async def test_finish_tells_deepgram_the_audio_is_over() -> None:
    socket = FakeSocket([])
    session = provider(socket).open()
    await session.push(b"\x00\x01")
    await session.finish()
    assert any("CloseStream" in str(item) for item in socket.sent)


@pytest.mark.asyncio
async def test_close_is_idempotent_and_closes_the_socket() -> None:
    socket = FakeSocket([])
    session = provider(socket).open()
    await session.push(b"\x00\x01")
    await session.close()
    await session.close()
    assert socket.closed


@pytest.mark.asyncio
async def test_a_send_failure_maps_to_asr_failed() -> None:
    socket = FakeSocket([], fail_on_send=True)
    session = provider(socket).open()
    with pytest.raises(ProviderError) as raised:
        await session.push(b"\x00\x01")
    assert raised.value.code is ErrorCode.ASR_FAILED

"""The ElevenLabs adapter, against a mocked transport.

**No paid call happens here.** `httpx.MockTransport` answers the request, so the real client code
path runs -- headers, params, body, streaming -- without leaving the process.
"""

from __future__ import annotations

from collections.abc import AsyncIterator, Callable

import httpx
import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ProviderError
from roboface_server.providers.elevenlabs import ElevenLabsProvider


def install(
    monkeypatch: pytest.MonkeyPatch, handler: Callable[[httpx.Request], httpx.Response]
) -> None:
    """Route every AsyncClient the adapter builds through a mock transport."""
    original = httpx.AsyncClient

    def factory(**kwargs: object) -> httpx.AsyncClient:
        return original(transport=httpx.MockTransport(handler), **kwargs)  # type: ignore[arg-type]

    monkeypatch.setattr(httpx, "AsyncClient", factory)


def streaming(chunks: list[bytes], status: int = 200) -> Callable[[httpx.Request], httpx.Response]:
    async def body() -> AsyncIterator[bytes]:
        for chunk in chunks:
            yield chunk

    def handler(request: httpx.Request) -> httpx.Response:
        handler.request = request  # type: ignore[attr-defined]
        return httpx.Response(status, content=body())

    return handler


def provider() -> ElevenLabsProvider:
    return ElevenLabsProvider("key-123", "voice-abc", "eleven_turbo_v2_5", "pcm_16000")


# --- construction ----------------------------------------------------------------------


def test_a_missing_key_is_an_enumerated_failure_not_a_traceback() -> None:
    with pytest.raises(ProviderError) as raised:
        ElevenLabsProvider("", "voice", "model", "pcm_16000")
    assert raised.value.code is ErrorCode.TTS_FAILED


def test_a_missing_voice_is_also_enumerated() -> None:
    with pytest.raises(ProviderError) as raised:
        ElevenLabsProvider("key", "", "model", "pcm_16000")
    assert raised.value.code is ErrorCode.TTS_FAILED


# --- the request -----------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_request_asks_for_the_streaming_endpoint_and_pcm(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    handler = streaming([b"\x00\x01"])
    install(monkeypatch, handler)
    async for _ in provider().synthesize("Привіт."):
        pass

    request = handler.request  # type: ignore[attr-defined]
    # The plain route would wait for the whole phrase before returning anything.
    assert request.url.path.endswith("/voice-abc/stream")
    assert request.url.params["output_format"] == "pcm_16000"
    assert request.headers["xi-api-key"] == "key-123"
    assert b"eleven_turbo_v2_5" in request.content
    assert "Привіт." in request.content.decode()


# --- streaming -------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chunks_arrive_as_several_not_one(monkeypatch: pytest.MonkeyPatch) -> None:
    install(monkeypatch, streaming([b"\x00\x01" * 4, b"\x00\x02" * 4, b"\x00\x03" * 4]))
    chunks = [chunk async for chunk in provider().synthesize("Привіт.")]
    assert len(chunks) == 3, "the adapter must not accumulate before yielding"


@pytest.mark.asyncio
async def test_an_odd_chunk_boundary_is_realigned(monkeypatch: pytest.MonkeyPatch) -> None:
    # PCM16 is two bytes per sample and the transport splits wherever it likes. Forwarding a chunk
    # that ends mid-sample shifts every following sample by a byte: the audio does not glitch once,
    # it turns to noise for the rest of the phrase.
    install(monkeypatch, streaming([b"\x01\x02\x03", b"\x04\x05\x06\x07"]))
    chunks = [chunk async for chunk in provider().synthesize("x")]

    assert all(len(chunk) % 2 == 0 for chunk in chunks), "every chunk must hold whole samples"
    # Nothing is reordered or lost in the middle: the concatenation is the input's even prefix.
    assert b"".join(chunks) == b"\x01\x02\x03\x04\x05\x06"


@pytest.mark.asyncio
async def test_a_trailing_half_sample_is_dropped(monkeypatch: pytest.MonkeyPatch) -> None:
    # One byte left at the end is half a sample. There is nothing to play it with, and padding it
    # would invent a sample the model never produced.
    install(monkeypatch, streaming([b"\x01\x02\x03"]))
    chunks = [chunk async for chunk in provider().synthesize("x")]
    assert b"".join(chunks) == b"\x01\x02"


@pytest.mark.asyncio
async def test_empty_chunks_are_skipped(monkeypatch: pytest.MonkeyPatch) -> None:
    install(monkeypatch, streaming([b"", b"\x00\x01", b""]))
    chunks = [chunk async for chunk in provider().synthesize("x")]
    assert chunks == [b"\x00\x01"]


# --- failures --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_server_error_becomes_tts_failed(monkeypatch: pytest.MonkeyPatch) -> None:
    # Deliberately *not* server_unreachable: DEVICE_UI renders a fault as its enumerated code, and
    # that one prints "No server" on a device that is connected to its server. (v0.2 review #3.)
    install(monkeypatch, streaming([b""], status=503))
    with pytest.raises(ProviderError) as raised:
        async for _ in provider().synthesize("x"):
            pass
    assert raised.value.code is ErrorCode.TTS_FAILED


@pytest.mark.asyncio
async def test_a_rate_limit_is_reported_as_one(monkeypatch: pytest.MonkeyPatch) -> None:
    install(monkeypatch, streaming([b""], status=429))
    with pytest.raises(ProviderError) as raised:
        async for _ in provider().synthesize("x"):
            pass
    assert raised.value.code is ErrorCode.RATE_LIMITED


@pytest.mark.asyncio
async def test_an_unauthorized_key_does_not_blame_the_device(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # *Our* key being wrong is not the *device* being unauthorized, and the device can do nothing
    # with that. It is a TTS failure, and the real cause is logged server-side.
    install(monkeypatch, streaming([b""], status=401))
    with pytest.raises(ProviderError) as raised:
        async for _ in provider().synthesize("x"):
            pass
    assert raised.value.code is ErrorCode.TTS_FAILED


@pytest.mark.asyncio
async def test_a_transport_failure_becomes_tts_failed(monkeypatch: pytest.MonkeyPatch) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        raise httpx.ConnectError("no route to host")

    install(monkeypatch, handler)
    with pytest.raises(ProviderError) as raised:
        async for _ in provider().synthesize("x"):
            pass
    assert raised.value.code is ErrorCode.TTS_FAILED

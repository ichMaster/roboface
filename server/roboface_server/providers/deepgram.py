"""Deepgram, over a WebSocket.

`wss://`, not batch REST, and that is the whole reason this file exists. Batch recognition cannot
start until the audio is complete, so it pays its latency *after* the person stops talking -- the
one moment they are actively waiting. Streaming pays it during, where it is free.

``websockets`` is imported inside the call, as the other two adapters import their dependencies: a
package-level import would make every test in the repository depend on it.
"""

from __future__ import annotations

import asyncio
import json
import os
from collections.abc import AsyncIterator, Awaitable, Callable
from contextlib import suppress
from typing import Any, Final
from urllib.parse import urlencode

from roboface_server.logging import log
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ASRChunk, ProviderError

#: Every frame the vendor sends, logged verbatim. A transcript that comes back empty looks
#: identical whether the vendor heard nothing, rejected the audio, or answered in a shape the
#: parser drops -- and only the raw frame separates those.
_TRACE: Final = os.environ.get("ROBOFACE_TRACE_ASR") == "1"

API_BASE: Final = "wss://api.deepgram.com/v1/listen"

#: How long Deepgram waits for silence before declaring a span final. Server-side, because the
#: server hears the audio at the same time we do and deciding here would add a round trip to every
#: decision.
DEFAULT_ENDPOINT_MS: Final = 500

#: The backstop, for when endpointing never fires -- a room with constant noise, or a speaker who
#: never quite stops. Without it a held phrase waits forever.
DEFAULT_UTTERANCE_END_MS: Final = 1000

#: Only what is unambiguous, the same policy the Gemini and ElevenLabs adapters follow: DEVICE_UI
#: renders a fault as its enumerated code, so blaming the device for a vendor problem prints the
#: wrong sentence on a screen.
_STATUS_HINTS: Final[dict[int, ErrorCode]] = {429: ErrorCode.RATE_LIMITED}


def build_listen_url(
    *,
    model: str,
    language: str,
    encoding: str,
    sample_rate: int,
    endpoint_ms: int = DEFAULT_ENDPOINT_MS,
    utterance_end_ms: int = DEFAULT_UTTERANCE_END_MS,
) -> str:
    """The listen URL, with every parameter this phase depends on.

    Its own function because **none of these fail loudly when wrong**. Omit ``interim_results`` and
    there are simply no interims, which looks like a slow network; get ``encoding`` wrong and the
    transcript is plausible nonsense. A test on the string is the only place these are visible.
    """
    query = {
        "model": model,
        "language": language,
        "encoding": encoding,
        "sample_rate": str(sample_rate),
        "channels": "1",
        "interim_results": "true",
        "smart_format": "true",
        "endpointing": str(endpoint_ms),
        "utterance_end_ms": str(utterance_end_ms),
    }
    return f"{API_BASE}?{urlencode(query)}"


#: How a socket is obtained. Injectable so tests drive a fake one -- without this every test of this
#: adapter is a paid network call, and none of them run in CI.
Connector = Callable[[str, dict[str, str]], Awaitable[Any]]


class DeepgramSession:
    """One recognition session over one socket."""

    def __init__(self, socket: Any) -> None:
        self._socket = socket
        self._closed = False

    async def push(self, audio: bytes) -> None:
        if self._closed:
            return
        try:
            await self._socket.send(audio)
        except Exception as exc:
            raise ProviderError(f"deepgram send failed: {exc}", ErrorCode.ASR_FAILED) from exc

    async def finish(self) -> None:
        """Tell Deepgram the audio is over. It may still emit finals after this."""
        if self._closed:
            return
        # A dead socket needs no closing message, and saying so would replace the real failure.
        with suppress(Exception):
            await self._socket.send(json.dumps({"type": "CloseStream"}))

    def results(self) -> AsyncIterator[ASRChunk]:
        return self._results()

    async def _results(self) -> AsyncIterator[ASRChunk]:
        try:
            async for message in self._socket:
                if _TRACE:
                    log("asr.raw", body=str(message)[:400])
                chunk = _parse_message(message)
                if chunk is not None:
                    yield chunk
        except ProviderError:
            raise
        except Exception as exc:
            raise ProviderError(f"deepgram stream failed: {exc}", ErrorCode.ASR_FAILED) from exc

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        # Closing a dead socket is not news.
        with suppress(Exception):
            await self._socket.close()


def _parse_message(raw: str | bytes) -> ASRChunk | None:
    """One Deepgram message into a chunk, or ``None`` when it carries no transcript.

    ``UtteranceEnd`` and ``Metadata`` arrive on the same socket and are not transcripts; treating
    an unrecognised message as an error would make a working vendor look broken, which is the same
    judgement `ws_protocol.h` makes about declared-but-unhandled types.
    """
    try:
        payload = json.loads(raw)
    except (ValueError, TypeError):
        return None
    if not isinstance(payload, dict):
        return None

    if payload.get("type") == "UtteranceEnd":
        # Signalled by an empty final: the tracker's backstop releases whatever is held.
        return ASRChunk(text="", is_final=True)

    alternatives = (payload.get("channel") or {}).get("alternatives") or []
    if not alternatives:
        return None
    transcript = alternatives[0].get("transcript")
    if not isinstance(transcript, str) or not transcript.strip():
        return None
    raw_confidence = alternatives[0].get("confidence")
    confidence = float(raw_confidence) if isinstance(raw_confidence, (int, float)) else 0.0
    return ASRChunk(
        text=transcript, is_final=bool(payload.get("is_final")), confidence=confidence
    )


class DeepgramProvider:
    """Streams transcripts from Deepgram as speech arrives."""

    def __init__(
        self,
        api_key: str,
        *,
        model: str,
        language: str,
        encoding: str,
        sample_rate: int,
        endpoint_ms: int = DEFAULT_ENDPOINT_MS,
        connector: Connector | None = None,
    ) -> None:
        if not api_key:
            raise ProviderError("DEEPGRAM_API_KEY is not set", ErrorCode.ASR_FAILED)
        self._api_key = api_key
        self._url = build_listen_url(
            model=model,
            language=language,
            encoding=encoding,
            sample_rate=sample_rate,
            endpoint_ms=endpoint_ms,
        )
        self._connector = connector

    def open(self) -> DeepgramSession:
        return DeepgramSession(_LazySocket(self._url, self._api_key, self._connector))


class _LazySocket:
    """Connects on first use, so ``open()`` can stay synchronous.

    The seam promises the caller holds a session before awaiting anything -- that is what lets the
    first audio frame carry its own budget. Connecting eagerly would make `open` a coroutine and
    move that choice into this adapter.
    """

    def __init__(self, url: str, api_key: str, connector: Connector | None) -> None:
        self._url = url
        self._api_key = api_key
        self._connector = connector
        self._socket: Any = None
        # Both directions call `_ensure`, and they run concurrently by design: the reader task
        # starts before the first frame is pushed. Without this lock each one saw `_socket` as None
        # and opened its **own** connection -- audio went to one socket and the reader listened on
        # the other, so nothing ever came back and nothing ever failed. It presents as a vendor that
        # silently ignores you.
        self._lock = asyncio.Lock()

    async def _ensure(self) -> Any:
        if self._socket is not None:
            return self._socket
        async with self._lock:
            # Re-check inside the lock: the task that waited here may find the socket already made.
            if self._socket is None:
                self._socket = await self._connect()
            return self._socket

    async def _connect(self) -> Any:
        headers = {"Authorization": f"Token {self._api_key}"}
        if self._connector is not None:
            return await self._connector(self._url, headers)
        try:
            import websockets
        except ImportError as exc:  # pragma: no cover -- environment problem
            raise ProviderError(
                "websockets is not installed; the Deepgram adapter needs it", ErrorCode.ASR_FAILED
            ) from exc
        try:
            return await websockets.connect(self._url, additional_headers=headers)
        except Exception as exc:
            raise ProviderError(f"deepgram connect failed: {exc}", ErrorCode.ASR_FAILED) from exc

    async def send(self, data: str | bytes) -> None:
        socket = await self._ensure()
        await socket.send(data)

    async def close(self) -> None:
        if self._socket is not None:
            await self._socket.close()

    def __aiter__(self) -> Any:
        return _LazyIterator(self)


class _LazyIterator:
    def __init__(self, lazy: _LazySocket) -> None:
        self._lazy = lazy
        self._inner: Any = None

    def __aiter__(self) -> Any:
        return self

    async def __anext__(self) -> Any:
        if self._inner is None:
            socket = await self._lazy._ensure()
            self._inner = socket.__aiter__()
        return await self._inner.__anext__()

"""The ASGI application: a WebSocket endpoint, and the adapter that feeds the router.

Everything framework-shaped lives here and nowhere else. ``protocol`` is pure, ``router`` is
written against a small transport protocol, and this module is the thin layer that makes a
FastAPI ``WebSocket`` look like one. That is what keeps the state machine testable without
an ASGI stack -- and what will let the WSS/TLS termination change without touching either.
"""

from __future__ import annotations

from collections.abc import MutableMapping
from contextlib import suppress
from typing import Any

import uvicorn
from fastapi import FastAPI, HTTPException, WebSocket
from starlette.websockets import WebSocketDisconnect, WebSocketState

from roboface_server.config import Settings, load_settings
from roboface_server.logging import configure, log
from roboface_server.protocol import FACE_SETS, ConfigUpdated
from roboface_server.router import (
    WS_CLOSE_NORMAL,
    ConnectionRegistry,
    Disconnected,
    EchoResponder,
    Responder,
    Router,
)

#: Where the device connects. One endpoint, one connection per device.
WS_PATH = "/ws"


class WebSocketTransport:
    """Adapts a FastAPI/Starlette ``WebSocket`` to the router's :class:`Transport`."""

    def __init__(self, websocket: WebSocket) -> None:
        self._websocket = websocket

    async def send(self, data: str) -> None:
        await self._websocket.send_text(data)

    async def send_bytes(self, data: bytes) -> None:
        """A binary frame -- `tts_audio` from v1.1, with no envelope by contract."""
        await self._websocket.send_bytes(data)

    async def receive(self) -> str | bytes:
        """The next frame, text or binary.

        Uses the raw ``receive`` rather than ``receive_text`` so that a binary frame arriving
        where text was expected is handed to the router as data -- the router has a rule for
        that (a binary frame with no phase to give it meaning) and should get to apply it,
        instead of the framework raising underneath it.
        """
        try:
            message: MutableMapping[str, Any] = await self._websocket.receive()
        except (WebSocketDisconnect, RuntimeError) as exc:
            # RuntimeError is what Starlette raises when receive() is called after the
            # socket has already closed -- indistinguishable, from here, from a disconnect.
            raise Disconnected(str(exc)) from exc

        if message.get("type") == "websocket.disconnect":
            raise Disconnected("client disconnected")

        text = message.get("text")
        if isinstance(text, str):
            return text
        payload = message.get("bytes")
        if isinstance(payload, bytes | bytearray):
            return bytes(payload)

        raise Disconnected(f"unexpected websocket message: {message.get('type')!r}")

    async def close(self, code: int = WS_CLOSE_NORMAL) -> None:
        """Close once, idempotently.

        The rejection path closes the socket inside the router and the endpoint closes again
        in its `finally`, so this is reached twice on every rejected `hello`. The state that
        matters is `application_state` -- what *this* side has already sent -- not
        `client_state`; guarding on the latter lets the second close through and Starlette
        raises "Cannot call send once a close message has been sent".
        """
        if self._websocket.application_state is WebSocketState.DISCONNECTED:
            return
        # The peer can still vanish between that check and this send.
        with suppress(RuntimeError):
            await self._websocket.close(code)


def build_responder(settings: Settings) -> Responder:
    """The real responder: the orchestrator, over Gemini.

    Imported here rather than at module scope so ``app.py`` stays importable — and the whole
    suite stays runnable — with ``google-genai`` absent. The key is demanded at **this**
    moment, which is the last one before a conversation could start and the first one at which
    its absence is unambiguous.
    """
    from roboface_server.orchestrator import Orchestrator
    from roboface_server.providers.gemini import GeminiProvider

    provider = GeminiProvider(
        settings.require_gemini_api_key(),
        model=settings.gemini_model,
        thinking_budget=settings.gemini_thinking_budget,
    )

    # Speech is optional, and its absence is a quiet device rather than a broken one. A missing
    # ElevenLabs key must not stop a text conversation working: v0's whole loop predates speech,
    # and a server that refused to start without a TTS key would make v1.1 a regression for
    # anyone who only wanted text.
    tts = None
    if settings.elevenlabs_api_key and settings.elevenlabs_voice_id:
        from roboface_server.providers.elevenlabs import ElevenLabsProvider

        tts = ElevenLabsProvider(
            settings.elevenlabs_api_key,
            settings.elevenlabs_voice_id,
            settings.elevenlabs_model,
            settings.elevenlabs_output_format,
            voice_settings=settings.elevenlabs_voice_settings,
        )
    else:
        log("tts.disabled", reason="no ELEVENLABS_API_KEY or ELEVENLABS_VOICE_ID", level="warning")

    # Recognition is optional in the same way speech is: without a key the device can still type,
    # which is what v0 and v1.1 were. Refusing to start would make v1.3 a regression for a
    # deployment that only wanted text.
    asr = None
    if settings.deepgram_api_key:
        from roboface_server.providers.deepgram import DeepgramProvider

        asr = DeepgramProvider(
            settings.deepgram_api_key,
            model=settings.deepgram_model,
            language=settings.deepgram_language,
            encoding=settings.deepgram_encoding,
            sample_rate=settings.deepgram_sample_rate,
            endpoint_ms=settings.deepgram_endpoint_ms,
        )
    else:
        log("asr.disabled", reason="no DEEPGRAM_API_KEY", level="warning")

    return Orchestrator(provider=provider, tts=tts, asr=asr)


def create_app(
    *,
    responder: Responder | None = None,
    registry: ConnectionRegistry | None = None,
    settings: Settings | None = None,
) -> FastAPI:
    """Build the application.

    A factory rather than a module-level ``app``: importing this module must not read
    ``server/.env`` or bind anything, and every test needs its own registry so two of them
    cannot see each other's connections.

    **An injected ``responder`` always wins**, and that is what keeps the suite free: tests
    pass a mock-backed orchestrator or the echo and never construct a real provider, so no key
    is needed and no network is reachable. Only when nothing is injected does this reach for
    configuration -- and then only if ``settings`` was supplied, because reaching for the
    environment on a bare ``create_app()`` would make an accidental call a paid one.
    """
    if responder is None:
        responder = build_responder(settings) if settings is not None else EchoResponder()

    application = FastAPI(title="RoboFace", version="2.6.1")
    router = Router(
        registry=registry if registry is not None else ConnectionRegistry(),
        responder=responder,
    )
    application.state.router = router

    @application.websocket(WS_PATH)
    async def websocket_endpoint(websocket: WebSocket) -> None:
        await websocket.accept()
        transport = WebSocketTransport(websocket)
        try:
            await router.serve(transport)
        finally:
            await transport.close()

    @application.post("/face/{face_set}")
    async def set_face(face_set: str) -> dict[str, object]:
        """Change the face of every connected device (v2.6).

        **The frame existed before anything sent it**, which is the third time this project has
        shipped that shape -- `TouchGestures::reset()` in v2.4, `frame_for(gaze=…)` in v2.5, and
        `config_updated` here. Each time the contract test passed, the codec round-tripped, and the
        feature did not exist. The roadmap's DoD for v2.6 is that the face is *"switchable both
        ways"*; without this endpoint only one of those ways was real.

        HTTP rather than a serial command because the server is on another machine (DEPLOYMENT.md)
        and the device is not reachable from the workstation at all -- an endpoint is the only way
        to exercise this seam from where the code is written.
        """
        if face_set not in FACE_SETS:
            raise HTTPException(
                status_code=404,
                detail=f"{face_set!r} is not a known face_set; try one of {sorted(FACE_SETS)}",
            )
        sent = await router.registry.broadcast(ConfigUpdated(face_set=face_set))
        return {"face_set": face_set, "devices": sent}

    return application


def main(settings: Settings | None = None) -> None:
    """Run the server on the configured host and port (``ROBOFACE_WS_*``).

    This is the entrypoint that actually talks to Gemini: it passes the loaded settings into
    the factory, so a missing ``GEMINI_API_KEY`` stops the process here with a clear message
    rather than surfacing as a failed turn once a device is already connected.
    """
    resolved = settings if settings is not None else load_settings()
    configure(resolved.log_level)
    uvicorn.run(
        create_app(settings=resolved),
        host=resolved.ws_host,
        port=resolved.ws_port,
        log_level=resolved.log_level,
    )


if __name__ == "__main__":  # pragma: no cover -- exercised by running the server
    main()

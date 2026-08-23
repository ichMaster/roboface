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
from fastapi import FastAPI, WebSocket
from starlette.websockets import WebSocketDisconnect, WebSocketState

from roboface_server.config import Settings, load_settings
from roboface_server.logging import configure
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
    return Orchestrator(provider=provider)


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

    application = FastAPI(title="RoboFace", version="0.2.1")
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

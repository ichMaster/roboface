"""Unit tests for the connection state machine.

Driven through a scripted transport rather than a socket. That is what makes the teardown
guarantees checkable at all: "the registry is empty after an exception" is exactly the kind
of claim that is true in review and false in production, and a fake transport can raise on
demand where a real one cannot.
"""

from __future__ import annotations

import json
from collections.abc import AsyncIterator

import pytest
from roboface_server.orchestrator import TurnAborted
from roboface_server.protocol import (
    AUDIO_FMT,
    PROTO_VERSION,
    Capability,
    ErrorCode,
    ErrorFrame,
    Hello,
    Ping,
    Reply,
    TextIn,
    decode,
    encode,
)
from roboface_server.router import (
    Connection,
    ConnectionPhase,
    ConnectionRegistry,
    Disconnected,
    EchoResponder,
    Router,
)


class _NoHistoryResponder:
    """A responder for tests that only care about frames, not about state."""

    def respond(self, session_id: str, text: str) -> AsyncIterator[str]:
        return self._stream(text)

    async def _stream(self, text: str) -> AsyncIterator[str]:
        yield text

    def forget(self, session_id: str) -> None:
        pass


class ScriptedTransport:
    """Hands the router a fixed list of inbound frames, then reports a disconnect."""

    def __init__(self, inbound: list[str | bytes] | None = None) -> None:
        self._inbound = list(inbound or [])
        self.sent: list[str] = []
        self.closed_with: int | None = None

    async def send(self, data: str) -> None:
        self.sent.append(data)

    async def receive(self) -> str | bytes:
        if not self._inbound:
            raise Disconnected("script exhausted")
        return self._inbound.pop(0)

    async def close(self, code: int = 1000) -> None:
        self.closed_with = code

    # -- helpers the assertions read through -------------------------------------------
    def frames(self) -> list[object]:
        return [decode(raw) for raw in self.sent]

    def errors(self) -> list[ErrorFrame]:
        return [frame for frame in self.frames() if isinstance(frame, ErrorFrame)]


class ExplodingTransport(ScriptedTransport):
    """Raises from ``receive`` -- an abrupt failure that is not a clean disconnect."""

    async def receive(self) -> str | bytes:
        raise RuntimeError("the socket caught fire")


def _hello(proto_ver: int = PROTO_VERSION, device_id: str = "core-s3-01") -> str:
    return encode(
        Hello(
            device_id=device_id,
            proto_ver=proto_ver,
            audio_fmt=AUDIO_FMT,
            caps=frozenset({Capability.TOUCH}),
        )
    )


def _router(registry: ConnectionRegistry | None = None) -> Router:
    return Router(
        registry=registry if registry is not None else ConnectionRegistry(),
        responder=EchoResponder(),
        session_id_factory=lambda: "session-under-test",
    )


# ---------------------------------------------------------------------------------------
# Greeting
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_frame_before_hello_is_an_error_not_a_crash() -> None:
    transport = ScriptedTransport([encode(TextIn(text="too early"))])

    await _router().serve(transport)

    assert [error.msg for error in transport.errors()] == ["the first frame must be 'hello'"]


@pytest.mark.asyncio
async def test_a_greeted_connection_becomes_ready() -> None:
    registry = ConnectionRegistry()
    router = _router(registry)
    seen: list[Connection] = []

    class Watching(ScriptedTransport):
        async def receive(self) -> str | bytes:
            seen.extend(registry.active())
            return await super().receive()

    transport = Watching([_hello(), encode(Ping())])
    await router.serve(transport)

    # Second look at the registry (before the ping) shows the connection accepted.
    assert seen[1].phase is ConnectionPhase.READY
    assert seen[1].device_id == "core-s3-01"
    assert seen[1].caps == frozenset({Capability.TOUCH})


@pytest.mark.asyncio
async def test_an_unsupported_proto_version_is_rejected_and_closed() -> None:
    transport = ScriptedTransport([_hello(proto_ver=PROTO_VERSION + 1)])

    await _router().serve(transport)

    errors = transport.errors()
    assert len(errors) == 1
    assert errors[0].code is ErrorCode.PROTO_UNSUPPORTED
    assert transport.closed_with == 1000


@pytest.mark.asyncio
async def test_a_rejected_device_gets_no_further_service() -> None:
    # The frames after the bad hello must not be answered: the connection is closing.
    transport = ScriptedTransport([_hello(proto_ver=99), encode(Ping()), encode(Ping())])

    await _router().serve(transport)

    assert len(transport.sent) == 1, "the router kept serving a rejected device"


@pytest.mark.asyncio
async def test_a_second_hello_on_a_ready_connection_is_rejected() -> None:
    transport = ScriptedTransport([_hello(), _hello(device_id="impostor")])

    await _router().serve(transport)

    assert [error.msg for error in transport.errors()] == ["'hello' was already negotiated"]


# ---------------------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ping_is_answered_with_pong() -> None:
    transport = ScriptedTransport([_hello(), encode(Ping())])

    await _router().serve(transport)

    assert [type(frame).__name__ for frame in transport.frames()] == ["Pong"]


@pytest.mark.asyncio
async def test_text_in_is_answered_through_the_injected_responder() -> None:
    class Shouting(_NoHistoryResponder):
        async def _stream(self, text: str) -> AsyncIterator[str]:
            for word in text.split():
                yield word.upper()

    router = Router(responder=Shouting(), session_id_factory=lambda: "s")
    transport = ScriptedTransport([_hello(), encode(TextIn(text="hello there"))])

    await router.serve(transport)

    assert transport.frames() == [
        Reply(text="HELLO", final=False),
        Reply(text="THERE", final=False),
        Reply(text="", final=True),
    ]


@pytest.mark.asyncio
async def test_the_default_responder_echoes_in_pieces() -> None:
    transport = ScriptedTransport([_hello(), encode(TextIn(text="привіт як справи"))])

    await _router().serve(transport)

    frames = transport.frames()
    deltas = [frame.text for frame in frames if isinstance(frame, Reply) and not frame.final]

    # More than one frame, and they reassemble exactly: the router forwarded each delta
    # unmodified and in order, and buffered nothing.
    assert len(deltas) > 1
    assert "".join(deltas) == "привіт як справи"
    assert frames[-1] == Reply(text="", final=True)


@pytest.mark.asyncio
async def test_the_responder_is_told_which_session_it_is_serving() -> None:
    # The seam passes session_id, not the connection: the responder needs to know which
    # conversation this is and nothing else about the socket.
    seen: list[str] = []

    class Recording(_NoHistoryResponder):
        def respond(self, session_id: str, text: str) -> AsyncIterator[str]:
            seen.append(session_id)
            return self._stream(text)

        async def _stream(self, text: str) -> AsyncIterator[str]:
            yield "ok"

    router = Router(responder=Recording(), session_id_factory=lambda: "session-under-test")
    await router.serve(ScriptedTransport([_hello(), encode(TextIn(text="x"))]))

    assert seen == ["session-under-test"]


@pytest.mark.asyncio
async def test_a_turn_that_aborts_sends_an_error_and_no_terminal_reply() -> None:
    """A `final: true` after a failure would present half a sentence as a finished answer."""

    class Failing(_NoHistoryResponder):
        async def _stream(self, text: str) -> AsyncIterator[str]:
            yield "почина"
            raise TurnAborted("provider died", ErrorCode.LLM_FAILED)

    router = Router(responder=Failing(), session_id_factory=lambda: "s")
    transport = ScriptedTransport([_hello(), encode(TextIn(text="x"))])

    await router.serve(transport)

    frames = transport.frames()
    assert frames[0] == Reply(text="почина", final=False)
    assert isinstance(frames[1], ErrorFrame)
    assert frames[1].code is ErrorCode.LLM_FAILED
    assert not any(isinstance(frame, Reply) and frame.final for frame in frames)


@pytest.mark.asyncio
@pytest.mark.parametrize("message_type", ["listen_start", "listen_stop", "event", "image_in"])
async def test_declared_but_unimplemented_types_get_a_clean_enumerated_error(
    message_type: str,
) -> None:
    transport = ScriptedTransport([_hello(), json.dumps({"type": message_type})])

    # The assertion that matters is that this returns at all: an unhandled exception here
    # would take the connection down instead of answering it.
    await _router().serve(transport)

    errors = transport.errors()
    assert len(errors) == 1
    assert errors[0].code in set(ErrorCode)


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "raw",
    ["not json", '{"type": "sing_a_song"}', '{"type": "text_in"}', '{"type": 7}'],
)
async def test_junk_is_answered_not_fatal(raw: str) -> None:
    transport = ScriptedTransport([_hello(), raw, encode(Ping())])

    await _router().serve(transport)

    kinds = [type(frame).__name__ for frame in transport.frames()]
    assert kinds == ["ErrorFrame", "Pong"], "the connection did not survive a bad frame"


@pytest.mark.asyncio
async def test_a_server_to_device_frame_from_a_device_is_rejected() -> None:
    # `decode` resolves both directions so one codec serves both tiers; direction is the
    # router's to enforce. A device sending `reply` is not having a conversation.
    transport = ScriptedTransport([_hello(), encode(Reply(text="I'll answer myself", final=True))])

    await _router().serve(transport)

    assert "server -> device" in transport.errors()[0].msg


@pytest.mark.asyncio
async def test_a_binary_frame_has_no_meaning_in_v0_1() -> None:
    # No listening window and no announced image exist yet, so nothing gives a raw payload
    # meaning. v1 turns this on by moving the connection into LISTENING, not by editing it.
    transport = ScriptedTransport([_hello(), b"\x00\x01\x02\x03"])

    await _router().serve(transport)

    assert "no meaning in this state" in transport.errors()[0].msg


# ---------------------------------------------------------------------------------------
# Teardown -- the guarantee that is easiest to believe without checking
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_teardown_on_a_normal_close() -> None:
    registry = ConnectionRegistry()

    await _router(registry).serve(ScriptedTransport([_hello(), encode(Ping())]))

    assert len(registry) == 0


@pytest.mark.asyncio
async def test_teardown_on_an_abrupt_disconnect() -> None:
    registry = ConnectionRegistry()

    # No frames at all: the client vanished between accept and its first send.
    await _router(registry).serve(ScriptedTransport([]))

    assert len(registry) == 0


@pytest.mark.asyncio
async def test_teardown_on_an_exception() -> None:
    registry = ConnectionRegistry()

    with pytest.raises(RuntimeError, match="caught fire"):
        await _router(registry).serve(ExplodingTransport())

    assert len(registry) == 0, "an exception leaked a connection into the registry"


@pytest.mark.asyncio
async def test_two_connections_are_tracked_separately_and_both_released() -> None:
    registry = ConnectionRegistry()
    ids = iter(["session-a", "session-b"])
    router = Router(
        registry=registry,
        responder=EchoResponder(),
        session_id_factory=lambda: next(ids),
    )

    await router.serve(ScriptedTransport([_hello(device_id="a")]))
    await router.serve(ScriptedTransport([_hello(device_id="b")]))

    assert len(registry) == 0


@pytest.mark.asyncio
async def test_teardown_tells_the_responder_to_forget_the_session() -> None:
    """Otherwise every connection that ever happened stays in memory for the process's life."""
    forgotten: list[str] = []

    class Counting(_NoHistoryResponder):
        def forget(self, session_id: str) -> None:
            forgotten.append(session_id)

    router = Router(responder=Counting(), session_id_factory=lambda: "session-under-test")
    await router.serve(ScriptedTransport([_hello(), encode(TextIn(text="hi"))]))

    assert forgotten == ["session-under-test"]


@pytest.mark.asyncio
async def test_the_session_is_forgotten_even_when_the_connection_errors() -> None:
    # The leak is worst on the failure path, which is the one nobody exercises by hand.
    forgotten: list[str] = []

    class Counting(_NoHistoryResponder):
        def forget(self, session_id: str) -> None:
            forgotten.append(session_id)

    router = Router(responder=Counting(), session_id_factory=lambda: "s")
    with pytest.raises(RuntimeError):
        await router.serve(ExplodingTransport())

    assert forgotten == ["s"]


def test_removing_a_connection_twice_is_safe() -> None:
    # Teardown runs from a `finally` that a later failure could re-enter.
    registry = ConnectionRegistry()
    connection = Connection(session_id="s")
    registry.add(connection)

    registry.remove(connection)
    registry.remove(connection)

    assert len(registry) == 0


# ---------------------------------------------------------------------------------------
# The application wiring
# ---------------------------------------------------------------------------------------


def test_the_app_exposes_the_websocket_endpoint() -> None:
    # The full turn over the real transport is RF-004's job (the fake device); this only
    # pins that the route exists and that the factory does not read configuration at import.
    from roboface_server.app import WS_PATH, create_app

    application = create_app()

    assert WS_PATH == "/ws"
    assert any(getattr(route, "path", None) == WS_PATH for route in application.routes)


def test_each_app_gets_its_own_registry() -> None:
    from roboface_server.app import create_app

    first = create_app()
    second = create_app()

    assert first.state.router.registry is not second.state.router.registry


# ---------------------------------------------------------------------------------------
# Whose fault it is (RF-009)
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("inbound", "what"),
    [
        (encode(TextIn(text="too early")), "a frame before hello"),
        ("{not json", "malformed JSON"),
        ('{"type": "sing_a_song"}', "an unknown type"),
        ('{"type": "emotion"}', "a declared but unimplemented type"),
        (encode(Reply(text="x", final=True)), "a server-to-device frame from a device"),
    ],
)
async def test_everything_the_router_rejects_is_the_devices_fault(
    inbound: str, what: str
) -> None:
    """`internal` would tell the device the server broke, which is a different thing.

    From v0.4 the device renders the error face for whatever code arrives, so this decides
    which side a person is shown as being at fault.
    """
    script = [_hello(), inbound] if "before hello" not in what else [inbound]
    transport = ScriptedTransport(script)

    await _router().serve(transport)

    errors = transport.errors()
    assert errors, f"{what} produced no error at all"
    assert errors[0].code is ErrorCode.BAD_FRAME, what


@pytest.mark.asyncio
async def test_a_binary_frame_is_the_devices_fault_too() -> None:
    transport = ScriptedTransport([_hello(), b"\x00\x01"])

    await _router().serve(transport)

    assert transport.errors()[0].code is ErrorCode.BAD_FRAME


@pytest.mark.asyncio
async def test_a_second_hello_is_the_devices_fault() -> None:
    transport = ScriptedTransport([_hello(), _hello(device_id="impostor")])

    await _router().serve(transport)

    assert transport.errors()[0].code is ErrorCode.BAD_FRAME

"""One connection state machine per device.

The router is the only place that knows a connection has a *history*: it greeted, it was
accepted, it is now exchanging frames, it is closing. ``protocol`` knows what a frame is;
the router knows whether this frame is allowed *now* and what to answer.

It is written against a small :class:`Transport` protocol rather than against FastAPI's
``WebSocket`` directly. That is not ceremony: it means the state machine -- including the
teardown guarantees, which are the easiest thing in a server to believe without checking --
is unit-testable with no ASGI stack, no event-loop plumbing and no real socket.

``text_in`` is answered through an injected :class:`Responder`. In v0.1 that is
:class:`EchoResponder`; **v0.2 replaces the injection with the orchestrator and the
``LLMProvider`` seam and nothing else in this module moves.** There is deliberately no
provider package, no streaming and no model call here -- the roadmap assigns those to v0.2,
and building the socket layer against a one-method seam is what keeps that swap honest.
"""

from __future__ import annotations

from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Protocol, runtime_checkable
from uuid import uuid4

from roboface_server.logging import bind_device, chars, connection_context, log
from roboface_server.orchestrator import TurnAborted
from roboface_server.protocol import (
    FRAME_TYPES,
    Accepted,
    BinaryPhase,
    Capability,
    DeviceMessage,
    ErrorCode,
    ErrorFrame,
    Frame,
    Hello,
    Ping,
    Pong,
    ProtocolError,
    Reply,
    TextIn,
    decode,
    device_binary_meaning,
    encode,
    negotiate,
)

#: Normal closure. A rejected ``hello`` still closes normally: the device is not broken, it
#: is simply speaking a version this server does not, and it should back off rather than
#: hammer a reconnect loop.
WS_CLOSE_NORMAL = 1000


class Disconnected(Exception):
    """The peer is gone. Raised by a transport's ``receive``; never an error condition."""


@runtime_checkable
class Transport(Protocol):
    """The socket, reduced to what the state machine actually needs."""

    async def send(self, data: str) -> None: ...

    async def receive(self) -> str | bytes: ...

    async def close(self, code: int = WS_CLOSE_NORMAL) -> None: ...


class Responder(Protocol):
    """How a ``text_in`` becomes a reply -- as a **stream of deltas**.

    v0.1 defined this as ``async def respond(text) -> str``, and v0.2 changed it: the reply is
    never accumulated and sent whole (ARCHITECTURE §Streaming is the architecture, not an
    optimisation). Returning the iterator rather than awaiting it is what lets each delta leave
    the moment it exists, which is the property v1's TTS stage depends on -- it synthesizes each
    completed phrase while the model is still generating.

    ``session_id`` rather than the whole connection: the responder needs to know *which*
    conversation this is, and nothing else about the socket.
    """

    def respond(self, session_id: str, text: str) -> AsyncIterator[str]: ...

    def forget(self, session_id: str) -> None:
        """Release whatever this session accumulated. Called once, at teardown.

        Part of the seam rather than the orchestrator's private business: a responder that
        keeps per-session state -- and from v0.2 the real one keeps conversation history --
        needs a defined moment to let it go, or every connection that ever happened stays in
        memory. The router owns that moment because it owns the connection's lifetime.
        """


class EchoResponder:
    """The router's own test double: it echoes, in pieces.

    Deliberately yields **several** deltas rather than one. A single-delta echo would let a
    buffering router pass every test in this file, which is precisely the regression the seam
    change exists to prevent.
    """

    def respond(self, session_id: str, text: str) -> AsyncIterator[str]:
        return self._stream(text)

    async def _stream(self, text: str) -> AsyncIterator[str]:
        for delta in echo_deltas(text):
            yield delta

    def forget(self, session_id: str) -> None:
        """Nothing to release -- an echo keeps no history."""


def echo_deltas(text: str) -> tuple[str, ...]:
    """Split ``text`` into the fragments the echo responder yields, preserving spacing.

    Word-sized, so joining the deltas reproduces the input exactly -- a test that asserts the
    round trip is then asserting the router forwarded every delta unmodified and in order.
    """
    if not text:
        return ()
    parts = text.split(" ")
    return tuple(part + " " if index < len(parts) - 1 else part for index, part in enumerate(parts))


class ConnectionPhase(StrEnum):
    """Where a connection is in its life. Every inbound frame is judged against it."""

    AWAITING_HELLO = "awaiting_hello"
    READY = "ready"
    CLOSING = "closing"


@dataclass(slots=True)
class Connection:
    """Per-connection state. One of these exists for as long as one socket does."""

    session_id: str
    phase: ConnectionPhase = ConnectionPhase.AWAITING_HELLO
    device_id: str | None = None
    caps: frozenset[Capability] = frozenset()

    def accept(self, hello: Hello) -> None:
        self.device_id = hello.device_id
        self.caps = hello.caps
        self.phase = ConnectionPhase.READY


class ConnectionRegistry:
    """The live connections.

    An instance, not module state: two tests running in one process must not be able to see
    each other's connections, and neither must two apps in one deployment.
    """

    def __init__(self) -> None:
        self._connections: dict[str, Connection] = {}

    def add(self, connection: Connection) -> None:
        self._connections[connection.session_id] = connection

    def remove(self, connection: Connection) -> None:
        # Discard rather than delete: teardown must be safe to reach twice, since it runs
        # from a `finally` that a later failure could re-enter.
        self._connections.pop(connection.session_id, None)

    def active(self) -> tuple[Connection, ...]:
        return tuple(self._connections.values())

    def __len__(self) -> int:
        return len(self._connections)


@dataclass(slots=True)
class Router:
    """Serves one connection at a time; holds no per-connection state of its own."""

    registry: ConnectionRegistry = field(default_factory=ConnectionRegistry)
    responder: Responder = field(default_factory=EchoResponder)
    session_id_factory: Callable[[], str] = field(default=lambda: uuid4().hex)

    async def serve(self, transport: Transport) -> None:
        """Run one connection to completion.

        The ``finally`` is the point of this method: whatever happens -- a clean close, a
        vanished client, an exception from a handler -- the connection leaves the registry.
        A server that leaks connection state survives its own tests and dies in a week.
        """
        connection = Connection(session_id=self.session_id_factory())
        with connection_context(connection.session_id):
            self.registry.add(connection)
            reason = "closed"
            try:
                log("connection.accepted", active=len(self.registry))
                while connection.phase is not ConnectionPhase.CLOSING:
                    try:
                        message = await transport.receive()
                    except Disconnected:
                        reason = "client_disconnected"
                        break
                    await self._handle(message, connection, transport)
                else:
                    reason = "rejected"
            except BaseException as exc:
                reason = f"error:{type(exc).__name__}"
                raise
            finally:
                self.registry.remove(connection)
                # The responder holds this session's conversation; without this every
                # connection that ever happened would stay in memory for the process's life.
                self.responder.forget(connection.session_id)
                log("connection.closed", reason=reason, active=len(self.registry))

    async def _handle(self, message: str | bytes, conn: Connection, transport: Transport) -> None:
        if isinstance(message, bytes | bytearray):
            log("frame.received", kind="binary", bytes=len(message), level="debug")
            await self._handle_binary(conn, transport)
            return

        try:
            frame = decode(message)
        except ProtocolError as exc:
            log("frame.rejected", code=str(exc.code), problem=type(exc).__name__, level="warning")
            # One branch for all three of MalformedFrame / UnknownMessage /
            # UnsupportedMessage: each already carries its own enumerated code, and the
            # router's job is to relay it rather than to re-classify it.
            await self._send_error(transport, exc.code, str(exc))
            return

        log("frame.received", kind=str(FRAME_TYPES[type(frame)]), level="debug")

        if not _is_from_device(frame):
            await self._send_error(
                transport,
                ErrorCode.INTERNAL,
                "that message type travels server -> device, not device -> server",
            )
            return

        if conn.phase is ConnectionPhase.AWAITING_HELLO:
            await self._handle_greeting(frame, conn, transport)
            return

        await self._dispatch(frame, conn, transport)

    async def _handle_binary(self, conn: Connection, transport: Transport) -> None:
        """A binary frame is meaningful only in a phase that gives it meaning.

        v0.1 has no such phase -- no listening window, no announced image -- so every binary
        frame is a violation. The predicate lives in ``protocol`` so v1 flips this on by
        moving the connection into ``LISTENING`` rather than by editing this branch.
        """
        meaning = device_binary_meaning(BinaryPhase.IDLE)
        if meaning is None:
            await self._send_error(
                transport,
                ErrorCode.INTERNAL,
                "a binary frame carries no envelope and has no meaning in this state",
            )

    async def _handle_greeting(self, frame: Frame, conn: Connection, transport: Transport) -> None:
        if not isinstance(frame, Hello):
            await self._send_error(
                transport, ErrorCode.INTERNAL, "the first frame must be 'hello'"
            )
            return

        outcome = negotiate(frame)
        if not isinstance(outcome, Accepted):
            log(
                "hello.rejected",
                proto_ver=frame.proto_ver,
                code=str(outcome.code),
                level="warning",
            )
            # Tell the device *why*, then close. Leaving it connected to a server that
            # cannot speak to it is the one outcome worse than refusing it.
            await self._send_error(transport, outcome.code, outcome.reason)
            conn.phase = ConnectionPhase.CLOSING
            await transport.close(WS_CLOSE_NORMAL)
            return

        conn.accept(outcome.hello)
        bind_device(outcome.hello.device_id)
        log(
            "hello.negotiated",
            proto_ver=outcome.hello.proto_ver,
            caps=sorted(str(cap) for cap in outcome.hello.caps),
            audio_fmt=outcome.hello.audio_fmt,
        )

    async def _dispatch(self, frame: Frame, conn: Connection, transport: Transport) -> None:
        match frame:
            case Ping():
                await transport.send(encode(Pong()))
            case TextIn():
                # chars(), never the text: a log has to stay safe to paste into an issue.
                log("turn.text_in", chars=chars(frame.text))
                await self._stream_reply(frame.text, conn, transport)
            case Hello():
                await self._send_error(
                    transport, ErrorCode.INTERNAL, "'hello' was already negotiated"
                )
            case _:
                await self._send_error(
                    transport, ErrorCode.INTERNAL, "message type is not handled in this phase"
                )

    async def _stream_reply(self, text: str, conn: Connection, transport: Transport) -> None:
        """Forward every delta as its own ``reply`` frame, then close the turn.

        **Nothing is buffered.** Each delta is sent inside the loop, before the next is even
        requested -- that is the difference between a streamed reply and a slow one, and it is
        what v1's playback path is built on.

        A turn that aborts sends ``error{code}`` and **no** terminal ``reply``: a
        ``final: true`` after a mid-stream failure would present half a sentence as a finished
        answer, and the device would have no way to know otherwise.
        """
        deltas = 0
        try:
            async for delta in self.responder.respond(conn.session_id, text):
                await transport.send(encode(Reply(text=delta, final=False)))
                deltas += 1
        except TurnAborted as exc:
            await self._send_error(transport, exc.code, str(exc))
            return

        await transport.send(encode(Reply(text="", final=True)))
        log("turn.streamed", deltas=deltas)

    async def _send_error(self, transport: Transport, code: ErrorCode, msg: str) -> None:
        log("error.sent", code=str(code), level="warning")
        await transport.send(encode(ErrorFrame(code=code, msg=msg)))


def _is_from_device(frame: Frame) -> bool:
    """Whether this frame belongs to the device -> server vocabulary.

    ``decode`` resolves against both directions so one codec serves both tiers; the router
    is where direction is enforced. A device sending ``reply`` is not having a conversation.
    """
    return isinstance(FRAME_TYPES[type(frame)], DeviceMessage)

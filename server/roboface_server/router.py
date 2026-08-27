"""One connection state machine per device.

The router is the only place that knows a connection has a *history*: it greeted, it was
accepted, it is now exchanging frames, it is closing. ``protocol`` knows what a frame is;
the router knows whether this frame is allowed *now* and what to answer.

It is written against a small :class:`Transport` protocol rather than against FastAPI's
``WebSocket`` directly. That is not ceremony: it means the state machine -- including the
teardown guarantees, which are the easiest thing in a server to believe without checking --
is unit-testable with no ASGI stack, no event-loop plumbing and no real socket.

``text_in`` is answered through an injected :class:`Responder`, which since v0.2 yields a
**stream of deltas** rather than a finished string. The orchestrator is the real
implementation and :class:`EchoResponder` is the router's own test double; this module knows
about neither Gemini nor prompts nor history, which is what building the socket layer against
a one-method seam bought.

**Every enumerated code this module chooses is ``bad_frame``.** Everything the router rejects
is a judgement about what the device sent; ``internal`` belongs to the server genuinely
breaking, and the ``llm_*`` codes arrive already classified from the turn.
"""

from __future__ import annotations

import hashlib
import time
from collections.abc import AsyncIterator, Callable
from contextlib import suppress
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Any, Protocol, runtime_checkable
from uuid import uuid4

from roboface_server.logging import bind_device, chars, connection_context, log
from roboface_server.orchestrator import TurnAborted
from roboface_server.protocol import (
    FRAME_TYPES,
    MAX_UTTERANCE_BYTES,
    Accepted,
    Asr,
    BinaryPhase,
    Capability,
    DeviceMessage,
    ErrorCode,
    ErrorFrame,
    Frame,
    Hello,
    ListenStart,
    ListenStop,
    Ping,
    Pong,
    ProtocolError,
    Reply,
    TextIn,
    TtsEnd,
    decode,
    device_binary_meaning,
    encode,
    negotiate,
)
from roboface_server.providers.base import ProviderError
from roboface_server.turn import AudioChunk, ReplyDelta, TurnEvent

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

    async def send_bytes(self, data: bytes) -> None:
        """A binary frame, with **no envelope**.

        Widened in v1.1. Until then the seam could only carry text, so there was no way to put
        audio on the wire at all -- the message types existed from v0.1 and the transport did not.
        Kept as a separate method rather than a `str | bytes` parameter because the two are
        different frame kinds on the wire, and a caller that got the union wrong would send a JSON
        string where the device expects PCM.
        """
        ...

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

    def respond(self, session_id: str, text: str) -> AsyncIterator[TurnEvent]: ...

    def open_listening(self) -> Any:
        """Begin recognising an utterance, or ``None`` when speech input is not configured.

        Optional in practice: a responder without it is a text-only server, which is what v0 and
        v1.1 were and what a deployment with no Deepgram key still is.
        """
        ...

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

    def respond(self, session_id: str, text: str) -> AsyncIterator[TurnEvent]:
        return self._stream(text)

    async def _stream(self, text: str) -> AsyncIterator[TurnEvent]:
        for delta in echo_deltas(text):
            yield ReplyDelta(text=delta)

    def open_listening(self) -> Any:
        """An echo hears nothing."""
        return None

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

    #: What an unlabelled binary frame means on this connection right now. `SPEAKING` is the only
    #: phase in which the server sends one, and the device is entitled to treat binary outside the
    #: window it was told about as a protocol violation.
    binary_phase: BinaryPhase = BinaryPhase.IDLE

    #: The utterance being assembled, while one is open. A list of chunks rather than a growing
    #: bytes object: joining once at the end is one allocation instead of one per frame, and a
    #: 30-second utterance is ~1500 frames.
    utterance: list[bytes] = field(default_factory=list)
    utterance_bytes: int = 0

    #: The recognition session, while one is open. Held per connection because a session belongs to
    #: one device's utterance and the orchestrator serves many.
    listening: Any = None

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
                # An utterance in flight dies with the socket. `forget` was v0.1's fix for
                # exactly this shape on the history side; the audio buffer is the same problem
                # with a bigger number attached -- up to MAX_UTTERANCE_BYTES held until the
                # Connection is collected, and a device on a flaky link reconnecting mid-sentence
                # accumulates one per attempt. The cap bounds a *live* utterance, not how many
                # dead ones may pile up.
                # An open vendor socket is the utterance buffer's problem with a bill attached.
                await self._close_listening(connection)
                self._end_utterance(connection)
                self.responder.forget(connection.session_id)
                log("connection.closed", reason=reason, active=len(self.registry))

    async def _handle(self, message: str | bytes, conn: Connection, transport: Transport) -> None:
        if isinstance(message, bytes | bytearray):
            log("frame.received", kind="binary", bytes=len(message), level="debug")
            await self._handle_binary(bytes(message), conn, transport)
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
                ErrorCode.BAD_FRAME,
                "that message type travels server -> device, not device -> server",
            )
            return

        if conn.phase is ConnectionPhase.AWAITING_HELLO:
            await self._handle_greeting(frame, conn, transport)
            return

        await self._dispatch(frame, conn, transport)

    async def _handle_binary(
        self, payload: bytes, conn: Connection, transport: Transport
    ) -> None:
        """A binary frame is meaningful only in a phase that gives it meaning.

        The predicate lives in ``protocol``, which is why v1.2 turned this on by moving the
        connection into ``LISTENING`` on ``listen_start`` rather than by editing this branch --
        exactly as v0.1's comment here anticipated.
        """
        meaning = device_binary_meaning(conn.binary_phase)
        if meaning is None:
            await self._send_error(
                transport,
                ErrorCode.BAD_FRAME,
                "a binary frame carries no envelope and has no meaning in this state",
            )
            return

        # Cap the utterance before storing, not after: checking afterwards means the memory has
        # already been taken, which is precisely what the cap exists to prevent.
        if conn.utterance_bytes + len(payload) > MAX_UTTERANCE_BYTES:
            log(
                "utterance.oversize",
                device_id=conn.device_id,
                session_id=conn.session_id,
                bytes=conn.utterance_bytes,
                limit=MAX_UTTERANCE_BYTES,
                level="warning",
            )
            self._end_utterance(conn)
            await self._send_error(
                transport,
                ErrorCode.BAD_FRAME,
                f"the utterance exceeded {MAX_UTTERANCE_BYTES} bytes and was ended",
            )
            return

        conn.utterance.append(payload)
        conn.utterance_bytes += len(payload)

        # Onward to the vendor **now**, while the person is still speaking. Holding these until
        # `listen_stop` would recreate batch recognition on top of a streaming transport.
        if conn.listening is not None:
            try:
                await conn.listening.push(payload)
            except ProviderError as exc:
                await self._abort_listening(conn, transport, exc)

    async def _abort_listening(
        self, conn: Connection, transport: Transport, exc: ProviderError
    ) -> None:
        """Recognition failed mid-utterance: close the window and tell the device which leg died."""
        log("listen.failed", problem=str(exc), level="warning")
        await self._close_listening(conn)
        self._end_utterance(conn)
        await self._send_error(
            transport, exc.code if exc.code is not None else ErrorCode.ASR_FAILED, str(exc)
        )

    async def _close_listening(self, conn: Connection) -> None:
        """Release the recognition session, whatever happened to it.

        An open vendor socket is the utterance buffer's problem with a bill attached, and v1.2
        already established that the buffer dies with the window.
        """
        session = conn.listening
        conn.listening = None
        if session is not None:
            with suppress(Exception):
                await session.close()

    def _end_utterance(self, conn: Connection) -> None:
        """Close the window and release what was collected.

        One place, because there are four ways an utterance ends -- stopped, oversize, a
        disconnect, and a fault -- and an invariant with four endings written four times is an
        invariant that will disagree with itself.
        """
        conn.binary_phase = BinaryPhase.IDLE
        conn.utterance.clear()
        conn.utterance_bytes = 0

    async def _handle_greeting(self, frame: Frame, conn: Connection, transport: Transport) -> None:
        if not isinstance(frame, Hello):
            await self._send_error(
                transport, ErrorCode.BAD_FRAME, "the first frame must be 'hello'"
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
            case ListenStart():
                if conn.binary_phase is BinaryPhase.LISTENING:
                    # Not a no-op. It means the device and the server disagree about state, and
                    # with one device and one developer that is worth surfacing rather than
                    # smoothing over -- a silently restarted window loses whatever preceded it.
                    await self._send_error(
                        transport, ErrorCode.BAD_FRAME, "already listening"
                    )
                    return
                conn.binary_phase = BinaryPhase.LISTENING
                conn.utterance.clear()
                conn.utterance_bytes = 0
                # Recognition begins with the window, not at its end: that is the whole of v1.3.
                opener = getattr(self.responder, "open_listening", None)
                conn.listening = opener() if opener is not None else None
                log("listen.start", device_id=conn.device_id, session_id=conn.session_id)
            case ListenStop():
                if conn.binary_phase is not BinaryPhase.LISTENING:
                    await self._send_error(
                        transport, ErrorCode.BAD_FRAME, "not listening"
                    )
                    return
                # v1.2 assembles and stops. v1.3 hands this to ASR; the seam is that the audio is
                # complete and in order at exactly this point, which is what the integration test
                # asserts and what v1.3 will depend on.
                # The digest is what makes "assembled intact" observable. v1.2 has nothing to
                # hand the audio to -- v1.3's ASR is where it goes -- so without this the only
                # evidence of correct assembly would be a byte count, which any reordering or
                # duplication would also satisfy. It is also useful in the field: two devices
                # reporting different digests for the same words is a capture bug, not an ASR one.
                assembled = b"".join(conn.utterance)
                stopped_at = time.monotonic()
                log(
                    "listen.stop",
                    device_id=conn.device_id,
                    session_id=conn.session_id,
                    bytes=conn.utterance_bytes,
                    frames=len(conn.utterance),
                    digest=hashlib.sha256(assembled).hexdigest()[:16],
                )
                session = conn.listening
                conn.listening = None
                self._end_utterance(conn)

                if session is None:
                    return  # a text-only deployment: the audio was accepted and goes nowhere

                try:
                    heard = await session.finish()
                except ProviderError as exc:
                    log("asr.failed", problem=str(exc), level="warning")
                    await self._send_error(
                        transport,
                        exc.code if exc.code is not None else ErrorCode.ASR_FAILED,
                        str(exc),
                    )
                    return

                # The number the DoD is about: how long resolution took *after* the audio stopped.
                # Everything before this happened while the person was talking and cost them
                # nothing, which is why this leg should be the smallest of the three.
                log("asr.resolved", ms=int((time.monotonic() - stopped_at) * 1000),
                    chars=chars(heard or ""))

                if not heard:
                    # Silence. v1.2's press-and-hold makes an empty utterance easy to produce, and
                    # the right answer to nothing is nothing.
                    log("asr.empty")
                    return

                await transport.send(encode(Asr(text=heard)))
                await self._stream_reply(heard, conn, transport, since=stopped_at)
            case Hello():
                await self._send_error(
                    transport, ErrorCode.BAD_FRAME, "'hello' was already negotiated"
                )
            case _:
                await self._send_error(
                    transport, ErrorCode.BAD_FRAME, "message type is not handled in this phase"
                )

    async def _stream_reply(
        self, text: str, conn: Connection, transport: Transport, since: float | None = None
    ) -> None:
        """Forward every delta as its own ``reply`` frame, then close the turn.

        **Nothing is buffered.** Each delta is sent inside the loop, before the next is even
        requested -- that is the difference between a streamed reply and a slow one, and it is
        what v1's playback path is built on.

        A turn that aborts sends ``error{code}`` and **no** terminal ``reply``: a
        ``final: true`` after a mid-stream failure would present half a sentence as a finished
        answer, and the device would have no way to know otherwise.
        """
        deltas = 0
        chunks = 0
        try:
            async for event in self.responder.respond(conn.session_id, text):
                match event:
                    case ReplyDelta():
                        if deltas == 0 and since is not None:
                            # The second of the three legs the DoD compares. Logged separately
                            # because "the ASR leg is the smallest" is a claim about three numbers,
                            # and one combined figure cannot support or refute it.
                            log("turn.first_delta_ms", ms=int((time.monotonic() - since) * 1000))
                        await transport.send(encode(Reply(text=event.text, final=False)))
                        deltas += 1
                    case AudioChunk():
                        # The phase is what gives an unlabelled binary frame its meaning
                        # (`server_binary_meaning`), so it is opened before the first chunk rather
                        # than at the start of the turn: a device that saw binary outside the
                        # speaking window would be right to treat it as a protocol violation.
                        if conn.binary_phase is not BinaryPhase.SPEAKING:
                            # Logged once per turn, and it is the DoD's evidence: this line must
                            # be timestamped *before* `turn.reply`, which the orchestrator writes
                            # after the model's last delta. If it is not, the pipeline is
                            # accumulating somewhere and the phase has not met its goal however
                            # good the audio sounds.
                            # The third leg. -1 when the turn was typed rather than spoken, so
                            # the field is always present and never silently absent.
                            log(
                                "turn.speaking",
                                deltas_so_far=deltas,
                                ms_since_listen_stop=(
                                    int((time.monotonic() - since) * 1000)
                                    if since is not None
                                    else -1
                                ),
                            )
                        conn.binary_phase = BinaryPhase.SPEAKING
                        await transport.send_bytes(event.data)
                        chunks += 1
        except TurnAborted as exc:
            await self._close_speaking(conn, transport)
            await self._send_error(transport, exc.code, str(exc))
            return
        except Exception as exc:
            # A bug in a responder or an untranslated vendor type would otherwise propagate
            # out of `serve` and drop the socket, giving the device nothing at all -- no
            # deltas, no error, just a dead connection it has to notice, back off from and
            # reconnect. ARCHITECTURE §Budgets and abort semantics is explicit that a failed
            # turn returns the device to idle and "the session stays connected".
            #
            # `internal` is the honest code here and the first thing to use it: the device's
            # frame was fine, the turn was fine, the *server* broke.
            #
            # `Exception`, never `BaseException` -- CancelledError must keep propagating, or
            # a shutdown would be swallowed and reported to the device as a server fault.
            log(
                "turn.crashed",
                problem=type(exc).__name__,
                deltas=deltas,
                level="error",
            )
            await self._close_speaking(conn, transport)
            await self._send_error(
                transport, ErrorCode.INTERNAL, f"the turn failed: {type(exc).__name__}"
            )
            return

        # The speaking window closes before the turn does: the device drains and gives the bus
        # back, then sees the turn end.
        await self._close_speaking(conn, transport)
        await transport.send(encode(Reply(text="", final=True)))
        log("turn.streamed", deltas=deltas, chunks=chunks)

    async def _close_speaking(self, conn: Connection, transport: Transport) -> bool:
        """End the speaking window, if one is open. Returns whether it did.

        Called on **every** exit from a turn, not only the successful one. The device switches its
        shared I2S bus from microphone to speaker when audio starts and switches back on
        `tts_end`; a turn that aborted after sending a chunk and never sent `tts_end` would leave
        the bus on the speaker and the device unable to listen -- a fault whose symptom appears one
        turn later, in a subsystem that is working correctly.
        """
        if conn.binary_phase is not BinaryPhase.SPEAKING:
            return False
        conn.binary_phase = BinaryPhase.IDLE
        await transport.send(encode(TtsEnd()))
        return True

    async def _send_error(self, transport: Transport, code: ErrorCode, msg: str) -> None:
        # Note every enumerated code this class chooses is `bad_frame`: everything the router
        # rejects is a judgement about what the device sent. `internal` belongs to the server
        # genuinely breaking, and `llm_*` codes arrive already classified from the turn.
        log("error.sent", code=str(code), level="warning")
        await transport.send(encode(ErrorFrame(code=code, msg=msg)))


def _is_from_device(frame: Frame) -> bool:
    """Whether this frame belongs to the device -> server vocabulary.

    ``decode`` resolves against both directions so one codec serves both tiers; the router
    is where direction is enforced. A device sending ``reply`` is not having a conversation.
    """
    return isinstance(FRAME_TYPES[type(frame)], DeviceMessage)

"""A device that speaks the WS protocol, for tests.

ARCHITECTURE §Testing and CI: "a **fake device** speaks the WS protocol in tests, and mock
Gemini/Deepgram/ElevenLabs return canned streams. CI never makes a paid call." This is that
fake device, and it is a long-lived asset rather than a fixture for one phase -- v1 pushes
binary PCM through it, v3 pushes JPEG, so its surface is *frames*, not `text_in`.

**It encodes and decodes exclusively through `protocol`.** That is the whole discipline of
the thing: a harness that hand-rolls its own JSON becomes a second, divergent implementation
of the wire, and then a contract change breaks production while the suite stays green. Here
a contract change breaks the harness loudly, which is the intended failure.

The transport is Starlette's `TestClient`, which drives the real ASGI application in a
portal thread. Frames therefore travel through the genuine websocket machinery -- the
adapter, the router, the codec -- and only the socket itself is substituted.
"""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from types import TracebackType
from typing import TypeVar

from fastapi import FastAPI
from roboface_server.app import WS_PATH
from roboface_server.protocol import (
    AUDIO_FMT,
    PROTO_VERSION,
    Capability,
    EmotionFrame,
    ErrorFrame,
    Frame,
    Hello,
    ListenStart,
    ListenStop,
    Reply,
    decode,
    encode,
)
from starlette.testclient import TestClient, WebSocketTestSession
from starlette.websockets import WebSocketDisconnect


@dataclass(frozen=True, slots=True)
class ReplyStream:
    """One turn's worth of `reply` frames, reassembled.

    `text` is what the person would hear; `deltas` is how it arrived. Assertions about
    streaming need the second, and a test that only ever looks at the first cannot tell a
    streamed reply from a buffered one.
    """

    text: str
    deltas: tuple[str, ...]
    #: The `emotion{}` frames that arrived during the turn, in order. Collected rather than
    #: discarded so a test that cares can assert them without a second read, and so a test that
    #: does not care is not made to.
    emotions: tuple[EmotionFrame, ...] = ()

    def __len__(self) -> int:
        return len(self.deltas)


@dataclass(frozen=True, slots=True)
class TurnStream:
    """One turn's frames and audio payloads, **in arrival order**.

    The order is the assertion. A server that collected a turn's audio and flushed it at the end
    emits the same set of frames as one that streams; only the sequence tells them apart.
    """

    events: tuple[Frame | bytes, ...]

    @property
    def audio(self) -> tuple[bytes, ...]:
        return tuple(event for event in self.events if isinstance(event, bytes))

    @property
    def frames(self) -> tuple[Frame, ...]:
        return tuple(event for event in self.events if not isinstance(event, bytes))

    @property
    def emotions(self) -> tuple[EmotionFrame, ...]:
        """The face channel, in order. From v2.2 a turn drives it as well as the reply."""
        return tuple(event for event in self.events if isinstance(event, EmotionFrame))

    def first_emotion_index(self) -> int | None:
        for index, event in enumerate(self.events):
            if isinstance(event, EmotionFrame):
                return index
        return None

    def first_delta_index(self) -> int | None:
        """Where the first `reply` delta sits.

        v2.2 compares this against the *replying* emotion frame: the face must change as the
        device begins to speak, so the frame has to precede the first word rather than follow it.
        """
        for index, event in enumerate(self.events):
            if isinstance(event, Reply) and not event.final:
                return index
        return None

    def first_audio_index(self) -> int | None:
        for index, event in enumerate(self.events):
            if isinstance(event, bytes):
                return index
        return None

    def last_delta_index(self) -> int | None:
        """Where the final `reply` delta sits. The DoD compares this against the first audio."""
        found = None
        for index, event in enumerate(self.events):
            if isinstance(event, Reply) and not event.final:
                found = index
        return found

    def __len__(self) -> int:
        return len(self.events)


FrameT = TypeVar("FrameT", bound=Frame)

# NOTE there is deliberately no per-receive timeout here. `WebSocketTestSession.receive_text`
# blocks on a queue, and wrapping each call in a thread with a deadline would leak the
# blocked thread on every timeout. The bound lives one level up instead: `pytest-timeout` in
# pyproject.toml turns a stalled server into a failing test with a traceback at the blocked
# line, rather than a CI job that hangs until its own wall-clock limit kills it. `recv_until`
# bounds how many frames are *skipped*, which is a different thing and not a substitute.


class FakeDevice:
    """One connected device. Send frames, receive frames, observe the close."""

    def __init__(self, session: WebSocketTestSession, *, device_id: str = "fake-core-s3") -> None:
        self._session = session
        self.device_id = device_id
        #: Every `emotion{}` `recv` skipped, in order. Kept rather than dropped so a test that
        #: wants to look at the face can, without having to read the socket a different way.
        self.emotions: list[EmotionFrame] = []

    # -- outbound ----------------------------------------------------------------------

    def send(self, frame: Frame) -> None:
        """Send a typed frame, encoded by `protocol`."""
        self._session.send_text(encode(frame))

    def send_raw(self, raw: str) -> None:
        """Send text that deliberately bypasses the codec -- for malformed-frame tests.

        The one sanctioned way around `protocol`, because "what does the server do with
        junk" cannot be asked through an encoder that refuses to produce junk.
        """
        self._session.send_text(raw)

    def utterance(self, pcm: bytes, *, chunk: int = 640) -> None:
        """Stream `pcm` the way a board does: a window, chunks, then a close.

        640 bytes is 20 ms at pcm16/16000/1 -- the frame size the firmware targets -- so a test
        that streams a second of audio produces the fifty frames a real utterance would, rather
        than one large one that would hide any per-frame bug.
        """
        self.send(ListenStart())
        for start in range(0, len(pcm), chunk):
            self.send_binary(pcm[start : start + chunk])
        self.send(ListenStop())

    def vad_utterance(self, pcm: bytes, *, chunk: int = 640, then_stop: bool = False) -> None:
        """Stream `pcm` the way an actively-listening board does: a window, chunks, **no close**.

        The difference from :meth:`utterance` is the whole of v1.4 on the wire. A board driven by
        its VAD opens the window when it hears speech and then waits: the recogniser ends the
        utterance and the board learns that from `asr`. It sends `listen_stop` only when its own
        end-pause elapses first, which is the backstop -- `then_stop` models that arriving late.
        """
        self.send(ListenStart())
        for start in range(0, len(pcm), chunk):
            self.send_binary(pcm[start : start + chunk])
        if then_stop:
            self.send(ListenStop())

    def send_binary(self, payload: bytes) -> None:
        """Send a raw binary frame -- no envelope, by contract. Audio from v1, JPEG from v3."""
        self._session.send_bytes(payload)

    def hello(
        self,
        *,
        proto_ver: int = PROTO_VERSION,
        audio_fmt: str = AUDIO_FMT,
        caps: frozenset[Capability] = frozenset(),
    ) -> None:
        """Greet the server. Always the first frame a real device sends."""
        self.send(
            Hello(
                device_id=self.device_id,
                proto_ver=proto_ver,
                audio_fmt=audio_fmt,
                caps=caps,
            )
        )

    # -- inbound -----------------------------------------------------------------------

    def recv(self) -> Frame:
        """The next frame from the server, **skipping `emotion{}`**.

        From v2.2 the server drives the face alongside everything else: a frame on `listen_start`,
        two before the first reply delta, one on a fault, one when a turn ends. A `recv` that
        returned them would turn every test in this suite into an assertion about the face --
        including the several dozen written before the face channel existed, none of which are
        about it.

        Where the face **is** the subject, `tests/contract/test_turn_lifecycle.py` reads it with
        `recv_including_emotion`, and `collect_turn`/`recv_any` keep it in arrival order, because
        the ordering — the `replying` frame before the first delta — is itself a contract.

        What was skipped is not thrown away: it accumulates on :attr:`emotions`.
        """
        while True:
            frame = self.recv_including_emotion()
            if isinstance(frame, EmotionFrame):
                self.emotions.append(frame)
                continue
            return frame

    def recv_including_emotion(self) -> Frame:
        """The next frame, whatever it is. The unfiltered view `recv` is built on."""
        return decode(self._session.receive_text())

    def recv_any(self) -> Frame | bytes:
        """The next frame, **text or binary**, in arrival order.

        `recv` decodes text and would choke on a binary frame; this one returns the raw payload
        instead. From v1.1 the server sends both on the same socket and the *order* between them
        is the property under test -- audio for an early phrase leaves while later text is still
        being generated -- so a helper that could only see one kind would make the thing this
        phase exists to prove untestable.
        """
        message = self._session.receive()
        payload = message.get("bytes")
        if payload is not None:
            return bytes(payload)
        text = message.get("text")
        if text is None:
            raise AssertionError(f"expected a text or binary frame, got {message}")
        return decode(text)

    def collect_turn(self, *, limit: int = 500) -> TurnStream:
        """Everything a turn emits, in arrival order, until the terminal `reply`.

        Returns the frames and the payloads interleaved exactly as they arrived. Asserting on
        that order is how a test distinguishes "streamed" from "collected then sent": a server
        that gathered all the audio and flushed it at the end produces the same *set* of frames
        and a completely different sequence.
        """
        events: list[Frame | bytes] = []
        for _ in range(limit):
            item = self.recv_any()
            events.append(item)
            if isinstance(item, ErrorFrame):
                return TurnStream(tuple(events))
            if isinstance(item, Reply) and item.final:
                return TurnStream(tuple(events))
        raise AssertionError(f"no terminal frame within {limit}; got {len(events)}")

    def recv_until(self, kind: type[FrameT], *, limit: int = 20) -> FrameT:
        """The next frame of type ``kind``, skipping anything before it.

        ``limit`` bounds the skipping so a server that never sends the expected frame fails
        the test with what it *did* send, rather than blocking until the suite times out.
        """
        seen: list[Frame] = []
        for _ in range(limit):
            frame = self.recv()
            if isinstance(frame, kind):
                return frame
            seen.append(frame)
        raise AssertionError(f"no {kind.__name__} within {limit} frames; got {seen}")

    def drain(self, count: int) -> list[Frame]:
        """Exactly ``count`` frames, in order. For asserting a stream."""
        return [self.recv() for _ in range(count)]

    def collect_reply(self, *, limit: int = 500) -> ReplyStream:
        """Read `reply` deltas until the terminal frame, and report what arrived.

        From v0.2 a turn is **many** `reply` frames -- deltas with `final: false`, closed by one
        `final: true`. Tests want two different things from that: the reassembled text, and the
        fact that it took more than one frame to say it. Returning both is what stops a test
        from asserting the text and accidentally passing against a buffered implementation.

        Raises if an `error` arrives instead: an aborted turn sends no terminal reply, so
        waiting for one would hang until the suite timeout rather than failing with the code.
        """
        deltas: list[str] = []
        emotions: list[EmotionFrame] = []
        for _ in range(limit):
            frame = self.recv()
            if isinstance(frame, ErrorFrame):
                raise AssertionError(f"turn aborted with {frame.code}: {frame.msg}")
            if isinstance(frame, EmotionFrame):
                # Collected, not asserted on. From v2.2 every turn drives the face as well as the
                # reply, and a helper called `collect_reply` that failed on a frame about the face
                # would make every test in the suite an assertion about v2.2's frame sequence --
                # including the several dozen written before that sequence existed.
                emotions.append(frame)
                continue
            if not isinstance(frame, Reply):
                raise AssertionError(f"expected a reply frame, got {frame}")
            if frame.final:
                return ReplyStream(
                    text="".join(deltas), deltas=tuple(deltas), emotions=tuple(emotions)
                )
            deltas.append(frame.text)
        raise AssertionError(f"no terminal reply within {limit} frames; got {len(deltas)} deltas")

    def expect_closed(self) -> None:
        """Assert the server has closed the socket.

        Used for the rejection path: a device speaking the wrong protocol version must not
        be left connected to a server that cannot talk to it.
        """
        try:
            self._session.receive_text()
        except WebSocketDisconnect:
            return
        raise AssertionError("the server left the socket open")

    # -- lifecycle ---------------------------------------------------------------------

    def close(self) -> None:
        self._session.close()

    def __enter__(self) -> FakeDevice:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()


@contextmanager
def connect(app: FastAPI, *, device_id: str = "fake-core-s3") -> Iterator[FakeDevice]:
    """Connect a fake device to ``app`` at the real endpoint, and disconnect on exit."""
    with TestClient(app) as client, client.websocket_connect(WS_PATH) as session:
        yield FakeDevice(session, device_id=device_id)

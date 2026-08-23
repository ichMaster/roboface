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
    ErrorFrame,
    Frame,
    Hello,
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

    def __len__(self) -> int:
        return len(self.deltas)


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
        """The next frame from the server, decoded by `protocol`."""
        return decode(self._session.receive_text())

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
        for _ in range(limit):
            frame = self.recv()
            if isinstance(frame, ErrorFrame):
                raise AssertionError(f"turn aborted with {frame.code}: {frame.msg}")
            if not isinstance(frame, Reply):
                raise AssertionError(f"expected a reply frame, got {frame}")
            if frame.final:
                return ReplyStream(text="".join(deltas), deltas=tuple(deltas))
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

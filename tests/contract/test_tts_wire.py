"""How synthesized audio reaches the device.

Pins two things the rest of v1.1 is built on: `tts_audio` is an **unlabelled binary frame** whose
meaning comes from the connection's phase, and the speaking window is **always** closed — including
when the turn dies, because a device that never sees `tts_end` leaves its shared I2S bus on the
speaker and cannot listen again.
"""

from __future__ import annotations

from collections.abc import AsyncIterator

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import TurnAborted
from roboface_server.protocol import ErrorCode, ErrorFrame, Reply, TextIn, TtsEnd
from roboface_server.turn import AudioChunk, ReplyDelta, TurnEvent

CHUNK_A = b"\x01\x02" * 8
CHUNK_B = b"\x03\x04" * 8


class SpeakingResponder:
    """Emits text and audio interleaved, the way the orchestrator does from RF-032."""

    def __init__(self, *, abort_after_audio: bool = False, silent: bool = False) -> None:
        self.abort_after_audio = abort_after_audio
        self.silent = silent

    def respond(self, session_id: str, text: str) -> AsyncIterator[TurnEvent]:
        return self._stream()

    async def _stream(self) -> AsyncIterator[TurnEvent]:
        yield ReplyDelta(text="Привіт")
        if self.silent:
            return
        yield AudioChunk(data=CHUNK_A)
        if self.abort_after_audio:
            raise TurnAborted("synthesis died mid-phrase", ErrorCode.TTS_FAILED)
        yield ReplyDelta(text=", світе.")
        yield AudioChunk(data=CHUNK_B)

    def forget(self, session_id: str) -> None:
        """Nothing to release."""


def _turn(responder: SpeakingResponder) -> object:
    with connect(create_app(responder=responder)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        return device.collect_turn()


def test_audio_arrives_as_binary_frames_carrying_the_payload_verbatim() -> None:
    turn = _turn(SpeakingResponder())
    assert turn.audio == (CHUNK_A, CHUNK_B), "binary frames carry the raw payload, no envelope"


def test_audio_and_text_interleave_rather_than_arriving_as_two_bursts() -> None:
    # The property the whole phase exists for. A server that collected the audio and flushed it
    # at the end emits the same *set* of frames; only the order tells them apart.
    turn = _turn(SpeakingResponder())

    def label(event: object) -> str:
        if isinstance(event, bytes):
            return "audio"
        if isinstance(event, TtsEnd):
            return "tts_end"
        if isinstance(event, Reply):
            return "final" if event.final else "reply"
        return type(event).__name__

    assert [label(event) for event in turn.events] == [
        "reply",
        "audio",
        "reply",
        "audio",
        "tts_end",
        "final",
    ]


def test_the_first_audio_precedes_the_last_text_delta() -> None:
    # This is the DoD's own check, asserted here against mocks so the hardware run confirms it
    # rather than discovering it.
    turn = _turn(SpeakingResponder())
    first_audio = turn.first_audio_index()
    last_delta = turn.last_delta_index()
    assert first_audio is not None and last_delta is not None
    assert first_audio < last_delta


def test_the_speaking_window_is_closed_before_the_turn_ends() -> None:
    turn = _turn(SpeakingResponder())
    frames = turn.frames
    assert isinstance(frames[-1], Reply) and frames[-1].final
    assert isinstance(frames[-2], TtsEnd), "tts_end closes the window before the turn closes"


def test_an_aborted_turn_still_closes_the_speaking_window() -> None:
    # Without this the device keeps the I2S bus on the speaker and cannot listen -- a fault whose
    # symptom appears one turn later, in a subsystem that is working correctly.
    turn = _turn(SpeakingResponder(abort_after_audio=True))
    frames = turn.frames
    assert isinstance(frames[-1], ErrorFrame)
    assert frames[-1].code is ErrorCode.TTS_FAILED
    assert any(isinstance(frame, TtsEnd) for frame in frames), "tts_end must precede the error"
    assert isinstance(frames[-2], TtsEnd)


def test_a_turn_with_no_audio_sends_no_tts_end() -> None:
    # The window is opened by the first chunk, not by the turn. A `tts_end` with nothing before it
    # would tell the device to drain a buffer it never filled and switch a bus it never switched.
    turn = _turn(SpeakingResponder(silent=True))
    assert turn.audio == ()
    assert not any(isinstance(frame, TtsEnd) for frame in turn.frames)

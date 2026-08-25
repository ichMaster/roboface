"""A turn that speaks, end to end, against mocks.

The assertion that matters is **order**. A server that collected a turn's audio and flushed it at
the end emits the same set of frames as one that streams; only the sequence distinguishes them, and
"speech starts before the model has finished generating" is the phase's stated goal.
"""

from __future__ import annotations

import pytest
from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator, TurnAborted
from roboface_server.protocol import ErrorCode, ErrorFrame, Reply, TextIn, TtsEnd
from roboface_server.providers.mock import DEFAULT_CHUNKS, MockLLMProvider, MockTTSProvider

# Two sentences, delivered as deltas that split a boundary from its whitespace -- the shape a real
# stream produces, and the one that catches a splitter emitting too early.
DELTAS = ("Привіт", ".", " Як", " спра", "ви?", " ")


def _app(llm: MockLLMProvider, tts: MockTTSProvider | None = None, **kwargs: object):
    orchestrator = Orchestrator(
        provider=llm, tts=tts if tts is not None else MockTTSProvider(), **kwargs
    )
    return create_app(responder=orchestrator)


def _turn(app, text: str = "привіт"):
    with connect(app) as device:
        device.hello()
        device.send(TextIn(text=text))
        return device.collect_turn()


def test_a_turn_emits_both_text_and_audio() -> None:
    turn = _turn(_app(MockLLMProvider(deltas=DELTAS)))
    assert turn.audio, "the turn produced no audio at all"
    assert any(isinstance(frame, Reply) and not frame.final for frame in turn.frames)


def test_audio_starts_before_the_model_finishes_writing() -> None:
    # The phase DoD, asserted against mocks so the hardware run confirms it rather than discovering
    # it: the first tts_audio must precede the *final* reply delta.
    turn = _turn(_app(MockLLMProvider(deltas=DELTAS)))
    first_audio = turn.first_audio_index()
    last_delta = turn.last_delta_index()
    assert first_audio is not None, "no audio was sent"
    assert last_delta is not None
    assert first_audio < last_delta, (
        "audio arrived only after the last text delta -- the pipeline is accumulating somewhere"
    )


def test_each_completed_phrase_is_synthesized_separately() -> None:
    tts = MockTTSProvider()
    _turn(_app(MockLLMProvider(deltas=DELTAS), tts))
    assert tts.calls == ["Привіт.", "Як справи?"], tts.calls


def test_a_phrase_is_never_a_half_word() -> None:
    # "the first spoken phrase is never a half-word" -- the splitter's whole job, seen from here.
    tts = MockTTSProvider()
    _turn(_app(MockLLMProvider(deltas=DELTAS), tts))
    for phrase in tts.calls:
        assert phrase == phrase.strip()
        assert not phrase.startswith((".", "?", "!", ";"))


def test_a_short_reply_with_no_terminal_punctuation_is_still_spoken() -> None:
    tts = MockTTSProvider()
    _turn(_app(MockLLMProvider(deltas=("Коротка", " відповідь")), tts))
    assert tts.calls == ["Коротка відповідь"], "the tail must be flushed and spoken"


def test_the_speaking_window_closes_before_the_turn_does() -> None:
    turn = _turn(_app(MockLLMProvider(deltas=DELTAS)))
    frames = turn.frames
    assert isinstance(frames[-1], Reply) and frames[-1].final
    assert isinstance(frames[-2], TtsEnd)


def test_a_tts_failure_aborts_the_turn_with_tts_failed() -> None:
    turn = _turn(_app(MockLLMProvider(deltas=DELTAS), MockTTSProvider(fail_at_index=0)))
    last = turn.frames[-1]
    assert isinstance(last, ErrorFrame)
    assert last.code is ErrorCode.TTS_FAILED


def test_a_tts_failure_rolls_the_question_out_of_history() -> None:
    orchestrator = Orchestrator(
        provider=MockLLMProvider(deltas=DELTAS), tts=MockTTSProvider(fail_at_index=0)
    )
    with connect(create_app(responder=orchestrator)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        device.collect_turn()
    assert orchestrator.history("") == () or all(
        message.text != "привіт" for message in orchestrator.history("")
    )


@pytest.mark.asyncio
async def test_a_stalled_first_chunk_fails_fast() -> None:
    # The budget sits on the first chunk of a phrase. A vendor that never starts must not hold the
    # turn open for its whole request timeout.
    orchestrator = Orchestrator(
        provider=MockLLMProvider(deltas=DELTAS),
        tts=MockTTSProvider(delay_s=0.5, delay_before_index=0),
        first_audio_budget_s=0.05,
    )
    events = []
    with pytest.raises(Exception) as raised:
        async for event in orchestrator.respond("s", "привіт"):
            events.append(event)
    assert "tts_failed" in str(getattr(raised.value, "code", "")) or events


def test_a_server_with_no_tts_still_answers_in_text() -> None:
    # Speech is added alongside the text, never instead of it. A missing TTS provider is a quiet
    # device, not a broken one -- v0's whole loop predates speech.
    orchestrator = Orchestrator(provider=MockLLMProvider(deltas=DELTAS), tts=None)
    with connect(create_app(responder=orchestrator)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()
    assert turn.audio == ()
    assert not any(isinstance(frame, TtsEnd) for frame in turn.frames)
    assert "".join(
        frame.text for frame in turn.frames if isinstance(frame, Reply)
    ) == "".join(DELTAS)


def test_audio_payloads_reach_the_device_verbatim() -> None:
    turn = _turn(_app(MockLLMProvider(deltas=("Привіт", ". "))))
    assert turn.audio == DEFAULT_CHUNKS


class ClosingTTS:
    """A TTS provider that records whether its stream was closed.

    The leak this catches is invisible to every other assertion: the audio is correct, the error
    code is correct, and an HTTP response stays open.
    """

    def __init__(self, *, fail_at: int | None = None, stall_s: float = 0.0) -> None:
        self.fail_at = fail_at
        self.stall_s = stall_s
        self.closed = 0
        self.opened = 0

    def synthesize(self, text: str):
        self.opened += 1
        return self._stream()

    async def _stream(self):
        import asyncio

        from roboface_server.protocol import ErrorCode as _EC
        from roboface_server.providers.base import ProviderError as _PE

        try:
            for index in range(4):
                if self.fail_at is not None and index == self.fail_at:
                    raise _PE("mock tts failure", _EC.TTS_FAILED)
                if self.stall_s and index == 0:
                    await asyncio.sleep(self.stall_s)
                yield b"\x00\x01" * 16
        finally:
            self.closed += 1


@pytest.mark.asyncio
async def test_a_failed_phrase_closes_its_tts_stream() -> None:
    # Otherwise each failure leaks an open HTTP response, and a vendor having a bad minute
    # exhausts the pool -- reporting tts_failed on turns whose synthesis would have worked.
    tts = ClosingTTS(fail_at=1)
    orchestrator = Orchestrator(provider=MockLLMProvider(deltas=DELTAS), tts=tts)

    with pytest.raises(TurnAborted):
        async for _ in orchestrator.respond("s", "привіт"):
            pass

    assert tts.opened > 0
    assert tts.closed == tts.opened, "a failed phrase left its stream open"


@pytest.mark.asyncio
async def test_a_stalled_phrase_closes_its_tts_stream() -> None:
    tts = ClosingTTS(stall_s=0.5)
    orchestrator = Orchestrator(
        provider=MockLLMProvider(deltas=DELTAS), tts=tts, first_audio_budget_s=0.05
    )

    with pytest.raises(TurnAborted):
        async for _ in orchestrator.respond("s", "привіт"):
            pass

    assert tts.closed == tts.opened, "a timed-out phrase left its stream open"


@pytest.mark.asyncio
async def test_a_successful_turn_closes_every_stream_it_opened() -> None:
    tts = ClosingTTS()
    orchestrator = Orchestrator(provider=MockLLMProvider(deltas=DELTAS), tts=tts)
    async for _ in orchestrator.respond("s", "привіт"):
        pass
    assert tts.opened >= 2, "the reply should have produced more than one phrase"
    assert tts.closed == tts.opened

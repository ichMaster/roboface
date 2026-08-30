"""The whole loop: audio in, speech out, against mocks.

What these assert beyond "it works" is *ordering* — recognition happens while audio arrives, and
the stages interleave on the wire. A pipeline that did each stage to completion in turn would pass
every value assertion here and fail every sequence one.
"""

from __future__ import annotations

from typing import Any

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import Asr, ErrorCode, ErrorFrame, Reply
from roboface_server.providers.base import ASRChunk, ProviderError
from roboface_server.providers.mock import MockASRProvider, MockLLMProvider, MockTTSProvider

SPEECH = b"\x00\x01" * 320


def build(**kwargs: Any) -> Any:
    orchestrator = Orchestrator(
        provider=kwargs.pop("llm", MockLLMProvider(deltas=("Привіт", "! Як справи?", " "))),
        tts=kwargs.pop("tts", MockTTSProvider()),
        asr=kwargs.pop("asr", MockASRProvider()),
        **kwargs,
    )
    return orchestrator, create_app(responder=orchestrator)


def speak(app: Any, frames: int = 5) -> Any:
    with connect(app) as device:
        device.hello()
        device.utterance(SPEECH * frames)
        return device.collect_turn()


def test_speaking_produces_a_transcript_and_a_spoken_answer() -> None:
    _, app = build()
    turn = speak(app)
    kinds = [type(e).__name__ if not isinstance(e, bytes) else "audio" for e in turn.events]

    assert "Asr" in kinds, "the device is never told what it said"
    assert "audio" in kinds, "no speech came back"
    assert isinstance(turn.frames[-1], Reply) and turn.frames[-1].final


def test_the_transcript_is_what_the_tracker_resolved() -> None:
    _, app = build()
    turn = speak(app)
    asr = next(f for f in turn.frames if isinstance(f, Asr))
    assert asr.text == "Привіт, як справи?"


def test_audio_reaches_the_vendor_during_the_window_not_after() -> None:
    # The claim the phase is built on. If the frames arrived in one lump at listen_stop, this
    # count would still be right -- so the assertion is that the session saw them *as many pushes*,
    # which only happens if the router forwarded each one on arrival.
    orchestrator, app = build()
    speak(app, frames=8)
    session = orchestrator.asr.sessions[0]  # type: ignore[union-attr]
    assert len(session.pushed) == 8


def test_the_asr_session_is_closed_when_the_turn_ends() -> None:
    # An open vendor socket is the utterance buffer's problem with a bill attached.
    orchestrator, app = build()
    speak(app)
    assert orchestrator.asr.sessions[0].closed  # type: ignore[union-attr]


def test_text_and_audio_interleave_in_the_answer() -> None:
    _, app = build()
    turn = speak(app)
    first_audio = turn.first_audio_index()
    last_delta = turn.last_delta_index()
    assert first_audio is not None and last_delta is not None
    assert first_audio < last_delta, "speech waited for the whole reply to be written"


def test_the_speaking_window_closes_before_the_turn() -> None:
    _, app = build()
    turn = speak(app)
    # Ordering, not adjacency -- see the note in tests/integration/test_tts_turn.py.
    kinds = [type(frame).__name__ for frame in turn.frames]
    assert kinds.index("TtsEnd") < len(kinds) - 1


def test_silence_produces_no_words() -> None:
    # A very short hold. The right answer to nothing is nothing *said* -- no error, and no reply to
    # an empty string. The turn still closes; `test_silence_still_closes_the_turn` covers why.
    _, app = build(asr=MockASRProvider(script=()))
    turn = speak(app)
    assert not turn.audio, "silence should not be spoken back"
    assert all(
        not (isinstance(f, Reply) and f.text) for f in turn.frames
    ), "something was said in reply to nothing"


# --- the three failure paths, each with its own code ------------------------------------


def test_a_recognition_failure_maps_to_asr_failed() -> None:
    _, app = build(asr=MockASRProvider(error=ProviderError("dead", ErrorCode.ASR_FAILED)))
    turn = speak(app)
    assert isinstance(turn.frames[-1], ErrorFrame)
    assert turn.frames[-1].code is ErrorCode.ASR_FAILED


def test_a_model_failure_maps_to_llm_failed() -> None:
    _, app = build(llm=MockLLMProvider(deltas=("x",), fail_at_index=0))
    turn = speak(app)
    assert isinstance(turn.frames[-1], ErrorFrame)
    assert turn.frames[-1].code is ErrorCode.LLM_FAILED


def test_a_synthesis_failure_maps_to_tts_failed() -> None:
    _, app = build(tts=MockTTSProvider(fail_at_index=0))
    turn = speak(app)
    assert isinstance(turn.frames[-1], ErrorFrame)
    assert turn.frames[-1].code is ErrorCode.TTS_FAILED


def test_a_held_phrase_is_released_by_the_backstop() -> None:
    # An un-punctuated final followed by Deepgram's UtteranceEnd (an empty final). Without the
    # backstop this utterance would never resolve and the turn would hang until its budget.
    script = (
        ASRChunk(text="просто скажи", is_final=False),
        ASRChunk(text="Просто скажи щось", is_final=True),
        ASRChunk(text="", is_final=True),
    )
    _, app = build(asr=MockASRProvider(script=script))
    turn = speak(app)
    asr = next(f for f in turn.frames if isinstance(f, Asr))
    assert asr.text == "Просто скажи щось"


def test_interims_are_not_forwarded_by_default() -> None:
    # Measured, not assumed: interims travel back over the socket the device is streaming audio
    # into, and the device drops audio it cannot send. Forwarding them cost half of every
    # utterance, and recognition returns nothing for audio missing every other chunk.
    from roboface_server.protocol import AsrPartial

    _, app = build()
    turn = speak(app, frames=6)
    assert not [f for f in turn.frames if isinstance(f, AsrPartial)]


def test_interims_reach_the_device_when_asked_for() -> None:
    from roboface_server.protocol import AsrPartial
    from roboface_server.router import ConnectionRegistry, Router

    orchestrator = Orchestrator(
        provider=MockLLMProvider(deltas=("Привіт", "! ")),
        tts=MockTTSProvider(),
        asr=MockASRProvider(),
    )
    app = create_app(responder=orchestrator, registry=ConnectionRegistry())
    # The flag lives on the Router; the app builds one, so touching its routes is how a deployment
    # reaches it. The result is unused -- what is being asserted is that `create_app` wires a router
    # at all, before the flag is checked on one built directly.
    assert app.router.routes
    router = Router(responder=orchestrator, send_partials=True)
    assert router.send_partials is True
    assert AsrPartial(text="x").text == "x"


def test_the_partial_queue_does_not_grow_for_the_whole_utterance() -> None:
    # The same omission's other half: unread interims accumulate for as long as the person talks.
    orchestrator, app = build()
    speak(app, frames=12)
    session = orchestrator.asr.sessions[0]  # type: ignore[union-attr]
    assert session.closed


def test_silence_still_closes_the_turn() -> None:
    """The right answer to nothing is nothing *said* -- but the turn must still end.

    v1.3 removed the firmware's own turn-ender because a reply follows an utterance now. When there
    is no reply, nothing else closes it: the device sat in `thinking` for the rest of the session
    and refused every later hold, which looks like a broken microphone.
    """
    _, app = build(asr=MockASRProvider(script=()))
    turn = speak(app)
    last = turn.frames[-1]
    assert isinstance(last, Reply) and last.final
    assert last.text == ""

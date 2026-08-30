"""The face channel, end to end, against the fake device.

The phase DoD is a claim about a *sequence*: "the face's expression comes from the server for every
turn and state change". A test that only checked the frames existed would pass against a server that
sent them all at the end, which is a face that never moved during the thing it was describing. So
what is asserted here is order, and specifically one ordering: **the `replying` frame precedes the
first reply delta.** If it followed, the device would speak with the previous expression and change
afterwards — correct, invisible, and impossible to notice from a log.

No paid call: every provider is mocked and the device is fake.
"""

from __future__ import annotations

import pytest
from fake_device import connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.emotion import ModelReport, TurnState
from roboface_server.emotion import frame_for as expected_frame
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import (
    Emotion,
    EmotionFrame,
    ErrorCode,
    ErrorFrame,
    ListenStart,
    Reply,
    TextIn,
)
from roboface_server.providers import MockLLMProvider, ProviderError
from roboface_server.providers.mock import MockTTSProvider
from roboface_server.router import ConnectionRegistry


def _app(provider: MockLLMProvider | None = None, **kwargs: object) -> FastAPI:
    orchestrator = Orchestrator(
        provider=provider if provider is not None else MockLLMProvider(),
        **kwargs,  # type: ignore[arg-type]
    )
    return create_app(responder=orchestrator, registry=ConnectionRegistry())


# --------------------------------------------------------------------------------------
# A whole turn
# --------------------------------------------------------------------------------------


def test_a_turn_drives_the_face_from_thinking_to_the_models_own_emotion() -> None:
    provider = MockLLMProvider(
        deltas=["Так", ", ", "звісно."], report=ModelReport(emotion="joy", intensity=0.9)
    )
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert [frame.emotion for frame in turn.emotions] == [
        Emotion.THINKING,  # the model was asked
        Emotion.JOY,       # the model answered, and said how it felt about it
        Emotion.NEUTRAL,   # the turn is over
    ]


def test_the_replying_frame_precedes_the_first_word() -> None:
    """The ordering the whole phase is arranged around.

    The response schema declares `emotion` before `reply` so the report arrives in the model's
    first chunk; the orchestrator yields the frame before the first delta so it reaches the wire
    first. Both of those exist to make this one assertion true.
    """
    with connect(_app(MockLLMProvider(deltas=["а", "б"]))) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    replying = next(
        index
        for index, event in enumerate(turn.events)
        if isinstance(event, EmotionFrame) and event.speaking
    )
    first_delta = turn.first_delta_index()

    assert first_delta is not None
    assert replying < first_delta


def test_the_models_intensity_reaches_the_device() -> None:
    provider = MockLLMProvider(report=ModelReport(emotion="sad", intensity=0.2))
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="сумна історія"))
        turn = device.collect_turn()

    speaking = [frame for frame in turn.emotions if frame.speaking]
    assert [(frame.emotion, frame.intensity) for frame in speaking] == [(Emotion.SAD, 0.2)]


def test_a_model_that_reports_nothing_still_drives_the_face() -> None:
    """A real case rather than a contrived one. The turn must not fail over a face."""
    with connect(_app(MockLLMProvider(report=None))) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert [frame.emotion for frame in turn.emotions] == [
        Emotion.THINKING,
        Emotion.NEUTRAL,  # the reply's own frame, with nothing reported
        Emotion.NEUTRAL,  # the end of the turn
    ]
    assert isinstance(turn.frames[-1], Reply)


def test_a_reported_emotion_outside_the_enum_never_reaches_the_screen() -> None:
    """The phase DoD, on the wire. The device is sent `neutral`, not `ecstatic`."""
    provider = MockLLMProvider(report=ModelReport(emotion="ecstatic", intensity=42))
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    for frame in turn.emotions:
        assert frame.emotion in set(Emotion)
        assert 0.0 <= frame.intensity <= 1.0


# --------------------------------------------------------------------------------------
# State changes outside a turn
# --------------------------------------------------------------------------------------


def test_opening_a_listening_window_sends_the_calm_face() -> None:
    """The one state change the orchestrator cannot see -- no turn exists yet. Attention is a
    face rather than a word on the screen (MISSION), so something has to send it."""
    with connect(_app()) as device:
        device.hello()
        device.send(ListenStart())
        frame = device.recv_including_emotion()

    assert isinstance(frame, EmotionFrame)
    assert frame.emotion is Emotion.CALM


def test_the_listening_frame_outlives_the_longest_utterance() -> None:
    """Otherwise the face would relax to `neutral` while someone was still mid-sentence."""
    with connect(_app()) as device:
        device.hello()
        device.send(ListenStart())
        frame = device.recv_including_emotion()

    assert isinstance(frame, EmotionFrame)
    assert frame.ttl_ms == expected_frame(TurnState.LISTENING).ttl_ms


# --------------------------------------------------------------------------------------
# Endings that are not a completed turn
# --------------------------------------------------------------------------------------


def test_a_failed_turn_leaves_the_error_face_and_not_a_stale_one() -> None:
    """A turn that aborted while `thinking` would otherwise hold that face for its whole ttl --
    fifteen seconds of a device apparently still working on an answer that failed."""
    provider = MockLLMProvider(fail_at_index=0, error=ProviderError("nope", ErrorCode.LLM_FAILED))
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.emotions[-1].emotion is Emotion.ERROR
    assert isinstance(turn.frames[-1], ErrorFrame)


def test_a_mid_stream_failure_also_ends_on_the_error_face() -> None:
    provider = MockLLMProvider(deltas=["почина", "ю"], fail_at_index=1)
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.emotions[-1].emotion is Emotion.ERROR


def test_the_error_face_precedes_the_error_code() -> None:
    """A person across the room sees the face; the code is for the band and for a log. The face
    should not arrive after the thing it is reacting to."""
    provider = MockLLMProvider(fail_at_index=0)
    with connect(_app(provider)) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    kinds = [type(event).__name__ for event in turn.events]
    assert kinds.index("EmotionFrame", kinds.count("EmotionFrame") - 1) < kinds.index("ErrorFrame")


def test_a_completed_turn_ends_by_asking_for_neutral() -> None:
    """**And it is a request, not a description.** The device is still playing seconds of audio
    this server has finished sending, so when to apply it is the device's decision -- the same
    division that v2.1.2 established for when a turn's face ends."""
    with connect(_app()) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    last = turn.emotions[-1]
    assert last.emotion is Emotion.NEUTRAL
    assert last.speaking is False


def test_the_face_is_driven_with_audio_in_the_turn_too() -> None:
    """The emotion frames must not be crowded out by the binary stream, or reordered behind it."""
    provider = MockLLMProvider(deltas=["Привіт. ", "Як справи?"])
    app = _app(provider, tts=MockTTSProvider())
    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.audio, "the turn produced no audio, so this asserts nothing"
    assert [frame.emotion for frame in turn.emotions] == [
        Emotion.THINKING,
        Emotion.JOY,
        Emotion.NEUTRAL,
    ]
    first_audio = turn.first_audio_index()
    replying = next(
        index
        for index, event in enumerate(turn.events)
        if isinstance(event, EmotionFrame) and event.speaking
    )
    assert first_audio is not None
    assert replying < first_audio, "the face changed after the device had started speaking"


@pytest.mark.parametrize("text", ["", "привіт", "x" * 500])
def test_every_turn_shape_drives_the_face(text: str) -> None:
    with connect(_app()) as device:
        device.hello()
        device.send(TextIn(text=text))
        turn = device.collect_turn()

    assert turn.emotions, f"no emotion frame for {text!r}"

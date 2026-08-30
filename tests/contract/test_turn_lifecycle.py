"""The turn lifecycle: what ends an utterance, and what a duplicate ending must not do.

§v1.4 moves the primary end-of-utterance decision from the device to the recogniser. That is a
change to the documented lifecycle in ARCHITECTURE.md §Turn lifecycle, so it is pinned here rather
than left to the integration tests -- the property is about *who decides*, and an integration test
that happens to pass would not say which end decided.
"""

from __future__ import annotations

from typing import Any

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import (
    Asr,
    Emotion,
    EmotionFrame,
    ErrorFrame,
    ListenStart,
    ListenStop,
    Reply,
)
from roboface_server.providers.mock import MockASRProvider, MockLLMProvider, MockTTSProvider

FRAME = b"\x01\x02" * 320


def build(**kwargs: Any) -> Any:
    orchestrator = Orchestrator(
        provider=kwargs.pop("llm", MockLLMProvider(deltas=("Добре.",))),
        tts=kwargs.pop("tts", MockTTSProvider()),
        asr=kwargs.pop("asr", MockASRProvider(settle_after=2)),
        **kwargs,
    )
    return orchestrator, create_app(responder=orchestrator)


def drain(device: Any) -> list[Any]:
    """Every JSON frame of one turn. `collect_turn` interleaves the binary audio; the lifecycle
    questions here are about frames, so the payloads are dropped rather than reasoned about."""
    return [event for event in device.collect_turn().events if not isinstance(event, bytes)]


def test_the_recogniser_ends_the_utterance_without_listen_stop() -> None:
    # The claim §v1.4 rests on: the recogniser hears the same silence the person made and calls it
    # while the audio is still arriving. The device sends no `listen_stop` here at all.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        frames = drain(device)

    assert any(isinstance(f, Asr) for f in frames), "the transcript never arrived"
    assert any(isinstance(f, Reply) and f.text for f in frames), "the turn never ran"


def test_a_late_listen_stop_is_not_a_second_turn() -> None:
    # Both ends are correct and simply noticed the silence at different moments: the recogniser
    # first, the device's end-pause a little later. The device must not be told it is wrong, and
    # the person must not be answered twice.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        first = drain(device)

        device.send(ListenStop())
        device.send(ListenStart())  # a probe: the connection must still be usable
        # Four frames, not two: the settle is noticed on the frame *after* the one that caused it,
        # which is how the router avoids polling. An utterance whose last frame settles is closed
        # by `listen_stop` instead -- covered by the backstop test below.
        for _ in range(4):
            device.send_binary(FRAME)
        second = drain(device)

    assert not any(isinstance(f, ErrorFrame) for f in first + second), (
        "a listen_stop for an utterance the recogniser already ended was treated as an error"
    )
    assert sum(1 for f in first if isinstance(f, Asr)) == 1
    assert sum(1 for f in second if isinstance(f, Asr)) == 1, "the next utterance was lost"


def test_listen_stop_still_ends_an_utterance_the_recogniser_never_settled() -> None:
    # The backstop. With no mid-window settling the device's end-pause is what closes the window,
    # exactly as in v1.3 -- so a recogniser that never endpoints does not strand the turn.
    _, app = build(asr=MockASRProvider())
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        device.send(ListenStop())
        frames = drain(device)

    assert any(isinstance(f, Asr) for f in frames)
    assert any(isinstance(f, Reply) and f.text for f in frames)


def test_listen_stop_with_no_window_is_still_an_error() -> None:
    # The tolerance added for the settled case must not become a general amnesty: a `listen_stop`
    # with no window and nothing settled is still the device and the server disagreeing about
    # state, which is worth surfacing.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStop())
        assert isinstance(device.recv(), ErrorFrame)


# ---------------------------------------------------------------------------------------
# The face channel's place in the lifecycle (v2.2)
# ---------------------------------------------------------------------------------------


def test_the_turn_lifecycle_drives_the_face_at_every_stage() -> None:
    """`emotion{}` is not an extra frame beside the turn -- it *is* the turn, seen from the screen.

    The sequence below is the phase DoD written as an ordering. Each frame answers a question the
    person is asking by looking at the device: has it heard me, is it working, what does it make of
    what it is saying, is it done.
    """
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        frames = drain(device)

    assert [f.emotion for f in frames if isinstance(f, EmotionFrame)] == [
        Emotion.CALM,      # listen_start: it has heard you
        Emotion.THINKING,  # the model was asked
        Emotion.JOY,       # the model answered, and said how it felt about the answer
        Emotion.NEUTRAL,   # the turn is over -- an instruction to relax, applied by the device
    ]


def test_the_replying_face_precedes_the_first_reply_delta() -> None:
    """The ordering the response schema's field order exists to produce.

    A frame arriving after the first word would be correct and useless: the device would speak the
    sentence wearing the previous expression and change once it had finished.
    """
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        frames = drain(device)

    kinds = [
        "speaking-face"
        if isinstance(f, EmotionFrame) and f.speaking
        else "delta"
        if isinstance(f, Reply) and f.text
        else "other"
        for f in frames
    ]
    assert kinds.index("speaking-face") < kinds.index("delta")


def test_the_relax_instruction_precedes_the_terminal_reply() -> None:
    """`reply{final: true}` is the turn's end marker on the wire, and a device is entitled to stop
    reading the turn there. A frame sent after it is a frame that may never be looked at."""
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        frames = drain(device)

    terminal = next(i for i, f in enumerate(frames) if isinstance(f, Reply) and f.final)
    relax = next(
        i
        for i, f in enumerate(frames)
        if isinstance(f, EmotionFrame) and f.emotion is Emotion.NEUTRAL
    )
    assert relax < terminal

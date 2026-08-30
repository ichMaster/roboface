"""v2.5 on the wire: a reported direction reaches the frames the server sends back.

**This file exists because of code review #2**, and the shape of that finding is worth keeping in
mind while reading it. `frame_for` grew a `gaze` parameter in RF-075 and nothing ever passed one, so
every frame sent during a turn carried `gaze: None` and erased the direction the device had just
reported. Every test passed: the contract test proved `EmotionFrame.gaze` round-trips, and the unit
tests proved `frame_for` puts a passed gaze into a frame. Neither could notice that no caller passed
one — a test of a function's behaviour cannot see that the function has no callers.

So these tests assert the *journey* rather than any one function: report a direction, run a turn,
and look at what came back.
"""

from __future__ import annotations

from fake_device import connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import Event, EventType, Ping, Pong, TextIn
from roboface_server.providers import MockLLMProvider


def _app() -> FastAPI:
    return create_app(responder=Orchestrator(provider=MockLLMProvider(deltas=("Добре.",))))


def test_a_reported_direction_rides_the_frames_of_the_next_turn() -> None:
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.VOICE, kind="direction", meta={"x": -0.6}))
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.emotions, "a turn sends emotion frames"
    for frame in turn.emotions:
        assert frame.gaze is not None, "the direction was erased by a frame that carried none"
        assert frame.gaze.x == -0.6


def test_no_reported_direction_means_no_opinion_rather_than_centre() -> None:
    """**Absence has to survive the journey too.**

    A server that filled in a centred gaze whenever it had none would override the device's idle
    drift and hold the eyes rigidly forward — which would look like a rendering bug and be a
    protocol one.
    """
    with connect(_app()) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.emotions
    assert all(frame.gaze is None for frame in turn.emotions)


def test_a_later_direction_replaces_the_earlier_one() -> None:
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.VOICE, kind="direction", meta={"x": -0.6}))
        device.send(Event(type=EventType.VOICE, kind="direction", meta={"x": 0.4}))
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.emotions
    assert all(frame.gaze is not None and frame.gaze.x == 0.4 for frame in turn.emotions)


def test_a_direction_is_answered_with_neither_a_face_nor_a_line() -> None:
    """Where a person is standing is not a mood and not a remark.

    A character that said something every time someone shifted in their chair would be unbearable,
    and a face change per direction update would put one on the wire fifty times a second.
    """
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.VOICE, kind="direction", meta={"x": -0.6}))
        # A ping afterwards proves the connection is alive and that nothing was queued ahead of
        # it -- the shape `test_being_picked_up_produces_nothing_at_all` uses, for the same reason.
        device.send(Ping())
        assert isinstance(device.recv_including_emotion(), Pong)


def test_nonsense_in_meta_means_no_opinion_rather_than_zero() -> None:
    """`meta` is free-form by contract, so this is arbitrary JSON off the network.

    Coercing an unusable value to 0.0 would be the server asserting a centred stare the device never
    claimed — and 0.0 is a perfectly valid direction, so the wrong default is indistinguishable from
    a real report.
    """
    for junk in ("left", None, [1, 2], True, {"x": 1}):
        with connect(_app()) as device:
            device.hello()
            device.send(Event(type=EventType.VOICE, kind="direction", meta={"x": junk}))
            device.send(TextIn(text="привіт"))
            turn = device.collect_turn()

        assert turn.emotions
        assert all(frame.gaze is None for frame in turn.emotions), f"{junk!r} became a gaze"

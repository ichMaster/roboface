"""A touch reaches the character, and sometimes the character answers.

The second level of the reaction model, end to end. The first level already happened on the device
before any of this was sent — that is the whole point of two levels — so what is tested here is
never the *speed* of a reaction. It is what the character makes of one.

**The commonest outcome is nothing, and it has its own tests.** A character that remarks on every
touch is one nobody wants on a desk, so "no frames at all" is a result rather than a gap.

No paid call: the LLM is mocked and the device is fake.
"""

from __future__ import annotations

import pytest
from fake_device import connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import (
    Emotion,
    EmotionFrame,
    Event,
    EventType,
    Reply,
    TextIn,
)
from roboface_server.providers import MockLLMProvider
from roboface_server.router import ConnectionRegistry


def _app(provider: MockLLMProvider | None = None) -> FastAPI:
    orchestrator = Orchestrator(provider=provider if provider is not None else MockLLMProvider())
    return create_app(responder=orchestrator, registry=ConnectionRegistry())


# --------------------------------------------------------------------------------------
# It reaches the character
# --------------------------------------------------------------------------------------


def test_a_stroke_produces_a_spoken_reaction() -> None:
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.TOUCH, kind="stroke", meta={"zone": "cheek"}))
        turn = device.collect_turn()

    assert any(isinstance(event, Reply) and event.text for event in turn.events)
    assert turn.emotions


def test_a_spoken_reaction_uses_the_ordinary_turn_path() -> None:
    """The same orchestrator, the same emotion frames, the same terminal reply.

    A second path to speech would be a second place for every latency, cancellation and history
    rule to be got wrong — so this asserts the shape of an ordinary turn rather than anything
    special about events.
    """
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.TOUCH, kind="stroke"))
        turn = device.collect_turn()

    assert isinstance(turn.frames[-1], Reply)
    assert turn.frames[-1].final
    assert [frame.emotion for frame in turn.emotions][-1] is Emotion.NEUTRAL


def test_a_tap_changes_the_face_and_says_nothing() -> None:
    """**The distinction the two tables exist for.** A tap deserves an expression, not a remark —
    the device already produced the reflex locally, and this is the server agreeing a moment later
    rather than repeating it."""
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.TOUCH, kind="tap", meta={"zone": "cheek"}))
        frame = device.recv_including_emotion()

        assert isinstance(frame, EmotionFrame)
        assert frame.emotion is Emotion.JOY


def test_being_picked_up_produces_nothing_at_all() -> None:
    """A device picked up and put down again does not need a line **or** a face. "Nothing" is a
    first-class outcome, and most of the reaction table is it."""
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=EventType.MOTION, kind="picked_up"))
        # A ping afterwards proves the connection is alive and that nothing was queued ahead of it.
        from roboface_server.protocol import Ping, Pong

        device.send(Ping())
        assert isinstance(device.recv_including_emotion(), Pong)


@pytest.mark.parametrize(
    ("event_type", "kind"),
    [
        (EventType.TOUCH, "stroke"),
        (EventType.TOUCH, "poke_eye"),
        (EventType.MOTION, "shake"),
        (EventType.MOTION, "free_fall"),
    ],
)
def test_the_events_worth_a_word_all_produce_one(event_type: EventType, kind: str) -> None:
    with connect(_app()) as device:
        device.hello()
        device.send(Event(type=event_type, kind=kind))
        turn = device.collect_turn()

    assert any(isinstance(event, Reply) and event.text for event in turn.events)


# --------------------------------------------------------------------------------------
# And it does not derail a turn
# --------------------------------------------------------------------------------------


def test_an_event_mid_turn_does_not_start_a_second_turn() -> None:
    """**The case this is most likely to be got wrong on.** Being stroked while the device is
    talking is not an edge case — it is what a person does — and half-duplex means there is nowhere
    to put a second answer."""
    with connect(_app(MockLLMProvider(deltas=["Привіт", ", ", "друже", "."]))) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        device.send(Event(type=EventType.TOUCH, kind="stroke"))
        turn = device.collect_turn()

    # Exactly one turn: one terminal reply, and the text is the one that was asked for.
    terminals = [e for e in turn.events if isinstance(e, Reply) and e.final]
    assert len(terminals) == 1
    spoken = "".join(e.text for e in turn.events if isinstance(e, Reply) and not e.final)
    assert spoken == "Привіт, друже."


def test_an_event_sent_during_a_turn_is_handled_after_it_rather_than_lost() -> None:
    """**A real limitation, asserted rather than wished away.**

    The router's receive loop is serial: `text_in` runs its whole turn before the next inbound frame
    is read, so an event arriving mid-reply is handled when the turn ends — not during it. The first
    version of this test asserted the face changed *during* the turn and failed, correctly.

    That is acceptable here and it is exactly why the model has two levels: **the reflex already
    fired on the device**, milliseconds after the finger landed and without consulting anyone. What
    arrives late is the character's comment, and a comment is allowed to be late.

    Concurrency during a turn is v3.4's, where barge-in needs it for its own reasons. Building it
    here would be that phase arriving early — and this test is what will notice when it lands.
    """
    with connect(_app()) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        device.send(Event(type=EventType.TOUCH, kind="poke_eye"))
        turn = device.collect_turn()

        # The turn itself is untouched...
        assert [frame.emotion for frame in turn.emotions][-1] is Emotion.NEUTRAL

        # ...and the poke is answered once it is over, rather than dropped.
        after = device.recv_including_emotion()

    assert isinstance(after, EmotionFrame)
    assert after.emotion is Emotion.SURPRISED


def test_an_unknown_kind_is_refused_rather_than_ignored() -> None:
    """The refusal `event{}` was built around, seen from the wire: a claim the contract does not
    define is a firmware bug, and the server saying so is how anyone finds out."""
    from roboface_server.protocol import ErrorCode, ErrorFrame

    with connect(_app()) as device:
        device.hello()
        device.send_raw('{"type":"event","event":{"type":"touch","kind":"lick"}}')
        frame = device.recv_including_emotion()

    assert isinstance(frame, ErrorFrame)
    assert frame.code is ErrorCode.BAD_FRAME

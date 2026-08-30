"""Contract test for `event{}` — the second level of the reaction model.

This file changes only when the contract changes. The authority is ARCHITECTURE.md §event{}.

**The decision this frame is here to pin is a refusal.** Every other decoder in `protocol.py`
validates and raises; `emotion{}` alone coerces. `event{}` goes back to validating, and the contrast
between the two is the content of both: a face is not worth dropping a connection over, but an event
is the *device making a claim about the physical world*, and a claim the contract does not define is
a firmware bug. Coercing it would turn that bug into a wrong reaction from the character — which
arrives as "why did it do that" rather than as an error, and is far harder to trace.
"""

from __future__ import annotations

import json

import pytest
from roboface_server.protocol import (
    EVENT_KINDS,
    MAX_EVENT_META_ENTRIES,
    DeviceMessage,
    Event,
    EventType,
    MalformedFrame,
    decode,
    encode,
)


def test_the_event_types_are_the_documented_four() -> None:
    assert [member.value for member in EventType] == ["touch", "motion", "proximity", "voice"]


def test_the_kind_vocabularies_are_the_documented_ones() -> None:
    """Spelled literally: a test that read these from the module could not notice it moving."""
    assert EVENT_KINDS[EventType.TOUCH] == frozenset(
        {"tap", "multi_tap", "stroke", "poke_eye", "long_press"}
    )
    assert EVENT_KINDS[EventType.MOTION] == frozenset(
        {"tilt", "shake", "picked_up", "upside_down", "free_fall"}
    )
    assert EVENT_KINDS[EventType.PROXIMITY] == frozenset({"approach", "leave"})
    assert EVENT_KINDS[EventType.VOICE] == frozenset({"direction"})


def test_the_documented_example_round_trips() -> None:
    frame = Event(type=EventType.TOUCH, kind="stroke", meta={"zone": "cheek", "count": 3})
    payload = json.loads(encode(frame))

    assert payload == {
        "type": "event",
        "event": {"type": "touch", "kind": "stroke", "meta": {"zone": "cheek", "count": 3}},
    }
    assert decode(encode(frame)) == frame


def test_the_event_type_is_nested_not_spread() -> None:
    """**The collision this nesting exists to avoid.**

    The envelope's `type` names the message — every frame in this protocol follows that, and
    `decode_envelope` depends on it. An event that spread its own `type` into the envelope would
    overwrite it, and the frame would decode as an unknown message called `touch`.
    """
    payload = json.loads(encode(Event(type=EventType.MOTION, kind="shake")))

    assert payload["type"] == DeviceMessage.EVENT
    assert payload["event"]["type"] == "motion"


def test_meta_is_optional_and_omitted_when_empty() -> None:
    payload = json.loads(encode(Event(type=EventType.PROXIMITY, kind="approach")))
    assert "meta" not in payload["event"]
    assert decode(encode(Event(type=EventType.PROXIMITY, kind="approach"))).meta == {}


# ---------------------------------------------------------------------------------------
# The refusals
# ---------------------------------------------------------------------------------------


@pytest.mark.parametrize("bad_type", ["lick", "TOUCH", "", "sound"])
def test_an_unknown_event_type_is_refused(bad_type: str) -> None:
    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "event", "event": {"type": bad_type, "kind": "tap"}}))


@pytest.mark.parametrize(
    ("event_type", "kind"),
    [("touch", "shake"), ("motion", "tap"), ("proximity", "stroke"), ("touch", "lick")],
)
def test_a_kind_from_the_wrong_type_is_refused(event_type: str, kind: str) -> None:
    """`shake` is a real kind — for motion. A touch that claims it is a firmware bug, and the
    server saying so is how anyone finds out."""
    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "event", "event": {"type": event_type, "kind": kind}}))


def test_a_missing_event_object_is_refused() -> None:
    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "event"}))
    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "event", "event": "touch"}))


def test_an_oversize_meta_is_refused() -> None:
    """Free-form is not unbounded. The device is trusted to be ours; the network is not, and a cap
    that is never reached costs nothing."""
    meta = {str(i): i for i in range(MAX_EVENT_META_ENTRIES + 1)}
    with pytest.raises(MalformedFrame):
        decode(
            json.dumps({"type": "event", "event": {"type": "touch", "kind": "tap", "meta": meta}})
        )


def test_a_meta_at_the_limit_is_accepted() -> None:
    meta = {str(i): i for i in range(MAX_EVENT_META_ENTRIES)}
    frame = decode(
        json.dumps({"type": "event", "event": {"type": "touch", "kind": "tap", "meta": meta}})
    )
    assert isinstance(frame, Event)
    assert len(frame.meta) == MAX_EVENT_META_ENTRIES


def test_a_non_object_meta_is_refused() -> None:
    with pytest.raises(MalformedFrame):
        decode(json.dumps({"type": "event", "event": {"type": "touch", "kind": "tap", "meta": []}}))


def test_event_refuses_where_emotion_coerces() -> None:
    """The two decisions side by side, because each is only defensible next to the other."""
    from roboface_server.protocol import Emotion, EmotionFrame

    coerced = decode('{"type":"emotion","emotion":"ecstatic","intensity":"loud"}')
    assert isinstance(coerced, EmotionFrame)
    assert coerced.emotion is Emotion.NEUTRAL

    with pytest.raises(MalformedFrame):
        decode('{"type":"event","event":{"type":"touch","kind":"ecstatic"}}')


def test_a_voice_direction_carries_its_x_through_meta() -> None:
    """v2.5. `voice` rides the event channel because it is the same kind of claim as the other
    three -- something the device sensed that the server could not have known -- even though it is
    not a thing that happened *to* the device."""
    frame = Event(type=EventType.VOICE, kind="direction", meta={"x": -0.62})
    payload = json.loads(encode(frame))

    assert payload["event"]["type"] == "voice"
    assert payload["event"]["kind"] == "direction"
    assert decode(encode(frame)).meta == {"x": -0.62}

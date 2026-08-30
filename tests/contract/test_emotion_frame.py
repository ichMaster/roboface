"""Contract test for the **`EmotionFrame`** seam.

This file changes only when the contract changes. Like the other contract tests it is deliberately
literal: the field names, the enum members and every default are spelled out here rather than read
from the module, because a test that asks the implementation what it does cannot notice the
implementation moving.

The authority is ARCHITECTURE.md §EmotionFrame. If this file and that section disagree, one of them
is wrong and the disagreement is the finding.

`EmotionFrame` is the second of the five seams and the one with two consumers that never meet: the
server builds it, the firmware renders it, and neither imports the other. The firmware's half is
pinned separately by `tests/contract/test_firmware_mirror.py` and by the C++ tests under
`firmware/test/`; those and this file are one contract checked from three sides.
"""

from __future__ import annotations

import json

import pytest
from roboface_server.protocol import (
    ACCENT_COLOR_RE,
    DEFAULT_INTENSITY,
    DEFAULT_TTL_MS,
    Emotion,
    EmotionFrame,
    Gaze,
    ServerMessage,
    decode,
    encode,
)

# --------------------------------------------------------------------------------------
# The enum
# --------------------------------------------------------------------------------------


def test_the_emotion_enum_is_exactly_the_documented_seven() -> None:
    """Seven values, spelled as documented, in the documented order.

    The order matters as documentation rather than as behaviour -- `neutral` first because it is
    what everything relaxes to. The membership matters absolutely: the firmware's recipe table is
    total over this enum, and an eighth value would render as a blank face rather than fail.
    """
    assert [member.value for member in Emotion] == [
        "neutral",
        "calm",
        "joy",
        "thinking",
        "surprised",
        "sad",
        "error",
    ]


def test_emotion_is_a_string_enum_on_the_wire() -> None:
    assert str(Emotion.JOY) == "joy"
    assert json.dumps({"emotion": Emotion.JOY}) == '{"emotion": "joy"}'


# --------------------------------------------------------------------------------------
# The frame's shape
# --------------------------------------------------------------------------------------


def test_the_documented_example_round_trips_byte_for_byte() -> None:
    """The exact object from ARCHITECTURE §EmotionFrame, encoded and read back."""
    frame = EmotionFrame(
        emotion=Emotion.JOY,
        intensity=0.8,
        gaze=Gaze(x=-0.4, y=0.0),
        accent_color="#5FFFC4",
        speaking=True,
        ttl_ms=6000,
    )
    payload = json.loads(encode(frame))
    assert payload == {
        "type": "emotion",
        "emotion": "joy",
        "intensity": 0.8,
        "gaze": {"x": -0.4, "y": 0.0},
        "accent_color": "#5FFFC4",
        "speaking": True,
        "ttl_ms": 6000,
    }
    assert decode(encode(frame)) == frame


def test_the_message_type_is_emotion() -> None:
    assert ServerMessage.EMOTION == "emotion"
    assert json.loads(encode(EmotionFrame(Emotion.CALM, 0.5)))["type"] == "emotion"


def test_only_emotion_and_intensity_are_required() -> None:
    """The other four carry documented defaults, and a frame at its defaults says only those two.

    Not a compression trick. A frame goes out on **every** state change, so a wire full of
    `"gaze":{"x":0,"y":0}` would bury the one field that actually moved.
    """
    payload = json.loads(encode(EmotionFrame(emotion=Emotion.NEUTRAL, intensity=0.3)))
    assert payload == {"type": "emotion", "emotion": "neutral", "intensity": 0.3}


def test_the_documented_defaults() -> None:
    frame = EmotionFrame(emotion=Emotion.NEUTRAL, intensity=0.0)
    assert frame.gaze is None  # "no opinion", not "look straight ahead"
    assert frame.accent_color is None  # means "use the recipe's own colour"
    assert frame.speaking is False
    assert frame.ttl_ms == DEFAULT_TTL_MS == 8000


def test_a_decoded_frame_with_no_optional_fields_takes_the_same_defaults() -> None:
    """The device applies these defaults too, which is why the encoder may omit them."""
    frame = decode('{"type":"emotion","emotion":"sad","intensity":0.4}')
    assert frame == EmotionFrame(emotion=Emotion.SAD, intensity=0.4)


def test_gaze_is_a_pair_in_minus_one_to_one() -> None:
    frame = decode('{"type":"emotion","emotion":"calm","intensity":0.5,"gaze":{"x":1,"y":-1}}')
    assert isinstance(frame, EmotionFrame)
    assert frame.gaze == Gaze(x=1.0, y=-1.0)


def test_accent_color_is_hash_rrggbb_and_nothing_else() -> None:
    assert ACCENT_COLOR_RE.match("#5FFFC4")
    assert ACCENT_COLOR_RE.match("#000000")
    for rejected in ("5FFFC4", "#5FFFC", "#5FFFC44", "#GGGGGG", "rgb(1,2,3)", "red", "#5fffc4 "):
        assert ACCENT_COLOR_RE.match(rejected) is None, rejected


# --------------------------------------------------------------------------------------
# The guarantee: untrusted input is always renderable
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize(
    "reported",
    ["happy", "HAPPY", "joyful", "", "neutral ", None, 7, [], {}, True],
    ids=["misspelt", "case", "synonym", "empty", "space", "none", "int", "list", "dict", "bool"],
)
def test_an_emotion_outside_the_enum_becomes_neutral(reported: object) -> None:
    """The phase DoD, stated as a test: a malformed model emotion never reaches the screen."""
    assert EmotionFrame.from_model(reported, 0.5).emotion is Emotion.NEUTRAL


@pytest.mark.parametrize(
    ("reported", "expected"),
    [
        (0.0, 0.0),
        (1.0, 1.0),
        (0.42, 0.42),
        (1, 1.0),
        (-3.0, 0.0),
        (99, 1.0),
        ("0.9", DEFAULT_INTENSITY),
        (None, DEFAULT_INTENSITY),
        (True, DEFAULT_INTENSITY),
        (float("nan"), DEFAULT_INTENSITY),
    ],
    ids=["zero", "one", "mid", "int", "under", "over", "str", "none", "bool", "nan"],
)
def test_intensity_is_always_a_usable_number(reported: object, expected: float) -> None:
    assert EmotionFrame.from_model(Emotion.JOY, reported).intensity == expected


def test_from_model_never_raises_whatever_it_is_given() -> None:
    """Totality, stated directly. A turn must not fail because a model was creative."""
    for emotion in ("joy", "happy", None, 3, object()):
        for intensity in (0.5, "x", None, float("inf"), object()):
            frame = EmotionFrame.from_model(emotion, intensity)
            assert isinstance(frame.emotion, Emotion)
            assert 0.0 <= frame.intensity <= 1.0


def test_a_malformed_emotion_frame_on_the_wire_is_coerced_rather_than_refused() -> None:
    """Unlike every other decoder, this one does not raise -- and that is deliberate.

    The two halves are separately releasable, so the device may not assume the server sanitised
    what it sent, and the server may not assume the device will. Both coerce. A face is also not
    worth dropping a connection over, which is what raising here would do.
    """
    frame = decode(
        '{"type":"emotion","emotion":"ecstatic","intensity":"very",'
        '"gaze":"left","accent_color":"chartreuse","ttl_ms":-5}'
    )
    assert frame == EmotionFrame(
        emotion=Emotion.NEUTRAL,
        intensity=DEFAULT_INTENSITY,
        gaze=None,
        accent_color=None,
        speaking=False,
        ttl_ms=DEFAULT_TTL_MS,
    )

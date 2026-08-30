"""The coercions that make untrusted model output safe to render.

`tests/contract/test_emotion_frame.py` pins *what the contract is*. This file covers *how the
coercions behave* -- including the debug trace each one leaves, which is the only way a prompt bug
becomes findable. A model that keeps reporting `"happy"` is not a protocol problem and will never
raise; without a log line it is invisible.

No LLM is called here, live or mocked: these are pure functions over values.
"""

from __future__ import annotations

import json
import logging

import pytest
from roboface_server.protocol import (
    DEFAULT_INTENSITY,
    DEFAULT_TTL_MS,
    Emotion,
    EmotionFrame,
    Gaze,
    coerce_accent_color,
    coerce_emotion,
    coerce_gaze,
    coerce_intensity,
    decode,
)

# --------------------------------------------------------------------------------------
# emotion
# --------------------------------------------------------------------------------------


def test_a_known_emotion_passes_through_unchanged() -> None:
    for member in Emotion:
        assert coerce_emotion(member.value) is member
        assert coerce_emotion(member) is member


def test_an_unknown_emotion_becomes_neutral_and_says_so(
    caplog: pytest.LogCaptureFixture,
) -> None:
    with caplog.at_level(logging.DEBUG, logger="roboface"):
        assert coerce_emotion("happy") is Emotion.NEUTRAL
    assert "happy" in caplog.text


def test_an_absent_emotion_becomes_neutral() -> None:
    assert coerce_emotion(None) is Emotion.NEUTRAL


def test_a_non_string_emotion_becomes_neutral() -> None:
    for value in (7, 1.5, [], {}, True, object()):
        assert coerce_emotion(value) is Emotion.NEUTRAL


# --------------------------------------------------------------------------------------
# intensity
# --------------------------------------------------------------------------------------


def test_intensity_in_range_passes_through() -> None:
    for value in (0.0, 0.25, 0.5, 1.0):
        assert coerce_intensity(value) == value


def test_intensity_out_of_range_is_clamped_and_logged(
    caplog: pytest.LogCaptureFixture,
) -> None:
    with caplog.at_level(logging.DEBUG, logger="roboface"):
        assert coerce_intensity(4.2) == 1.0
        assert coerce_intensity(-1.0) == 0.0
    assert "clamped" in caplog.text


def test_a_non_numeric_intensity_becomes_the_documented_default() -> None:
    for value in ("0.9", None, [], {}, object()):
        assert coerce_intensity(value) == DEFAULT_INTENSITY


def test_a_boolean_intensity_is_not_a_number() -> None:
    """`bool` is an `int` subclass, so `True` would otherwise arrive as full intensity."""
    assert coerce_intensity(True) == DEFAULT_INTENSITY
    assert coerce_intensity(False) == DEFAULT_INTENSITY


def test_nan_intensity_becomes_the_default_rather_than_surviving_the_clamp() -> None:
    """NaN fails every comparison, so `min`/`max` would pass it straight through."""
    assert coerce_intensity(float("nan")) == DEFAULT_INTENSITY


def test_infinite_intensity_clamps() -> None:
    assert coerce_intensity(float("inf")) == 1.0
    assert coerce_intensity(float("-inf")) == 0.0


# --------------------------------------------------------------------------------------
# gaze
# --------------------------------------------------------------------------------------


def test_absent_gaze_stays_absent() -> None:
    """`None` and centre are different instructions once a device reflex can override one."""
    assert coerce_gaze(None) is None


def test_a_well_formed_gaze_passes_through() -> None:
    assert coerce_gaze({"x": -0.4, "y": 0.25}) == Gaze(x=-0.4, y=0.25)


def test_gaze_components_clamp_independently() -> None:
    assert coerce_gaze({"x": -9.0, "y": 9.0}) == Gaze(x=-1.0, y=1.0)


def test_a_missing_axis_is_centre_not_a_failure() -> None:
    assert coerce_gaze({"x": 0.5}) == Gaze(x=0.5, y=0.0)


def test_a_non_object_gaze_is_dropped(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.DEBUG, logger="roboface"):
        assert coerce_gaze("left") is None
        assert coerce_gaze([0.5, 0.5]) is None
    assert "gaze" in caplog.text


def test_a_non_numeric_axis_is_centre() -> None:
    assert coerce_gaze({"x": "left", "y": True}) == Gaze(x=0.0, y=0.0)


# --------------------------------------------------------------------------------------
# accent_color
# --------------------------------------------------------------------------------------


def test_a_well_formed_colour_passes_through_in_either_case() -> None:
    assert coerce_accent_color("#5FFFC4") == "#5FFFC4"
    assert coerce_accent_color("#5fffc4") == "#5fffc4"


def test_a_malformed_colour_is_dropped_not_defaulted(
    caplog: pytest.LogCaptureFixture,
) -> None:
    """Absent means "use the recipe's own colour" -- a working face. An invented colour would be a
    decision the skin never agreed to."""
    with caplog.at_level(logging.DEBUG, logger="roboface"):
        for value in ("red", "#GGGGGG", "#5FFFC", "5FFFC4", 0x5FFFC4, ["#5FFFC4"]):
            assert coerce_accent_color(value) is None
    assert "accent_color" in caplog.text


def test_an_absent_colour_is_silent(caplog: pytest.LogCaptureFixture) -> None:
    """Omitting the field is the normal case and must not fill a log with it."""
    with caplog.at_level(logging.DEBUG, logger="roboface"):
        assert coerce_accent_color(None) is None
    assert caplog.text == ""


# --------------------------------------------------------------------------------------
# ttl, and the whole frame
# --------------------------------------------------------------------------------------


def _wire(**fields: object) -> EmotionFrame:
    """One `emotion{}` frame off the wire, built the way a device would receive it."""
    frame = decode(json.dumps({"type": "emotion", "emotion": "joy", "intensity": 0.5} | fields))
    assert isinstance(frame, EmotionFrame)
    return frame


@pytest.mark.parametrize("value", ["8000", -1, 0, None, True, 1.5], ids=str)
def test_an_unusable_ttl_becomes_the_default(value: object) -> None:
    """Zero and negative included: a ttl that has already expired is a face that never appears."""
    assert _wire(ttl_ms=value).ttl_ms == DEFAULT_TTL_MS


def test_a_positive_ttl_passes_through() -> None:
    assert _wire(ttl_ms=1200).ttl_ms == 1200


def test_speaking_is_true_only_when_it_is_literally_true() -> None:
    """A truthy value is not the same as the flag. `speaking` gates the mouth on the device, and
    "1" arriving as `True` would be a decision made by a coincidence of Python."""
    assert _wire(speaking=True).speaking is True
    assert _wire(speaking=1).speaking is False
    assert _wire(speaking="yes").speaking is False
    assert _wire().speaking is False


def test_from_model_composes_every_coercion() -> None:
    frame = EmotionFrame.from_model(
        "elated", "loud", gaze={"x": 4, "y": "up"}, accent_color="chartreuse", speaking=True
    )
    assert frame == EmotionFrame(
        emotion=Emotion.NEUTRAL,
        intensity=DEFAULT_INTENSITY,
        gaze=Gaze(x=1.0, y=0.0),
        accent_color=None,
        speaking=True,
        ttl_ms=DEFAULT_TTL_MS,
    )

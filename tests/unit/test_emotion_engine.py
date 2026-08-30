"""The emotion engine's mapping.

Pure functions over values -- no turn, no socket, no provider, and no LLM called live or mocked.
That is the point of keeping the decision in its own module: the question "what should the face do
when the model says it is sad?" is answerable in a millisecond and without a network.

The totality tests here are the ones that matter. A state or an emotion the table has no entry for
does not raise on the server; it produces a frame the firmware has no recipe for, and *that* is a
blank face on a desk with no stack trace anywhere.
"""

from __future__ import annotations

import pytest
from roboface_server.emotion import (
    ACCENTS,
    BASE_INTENSITY,
    STATE_EMOTION,
    TTL_MS,
    ModelReport,
    TurnState,
    frame_for,
)
from roboface_server.protocol import ACCENT_COLOR_RE, DEFAULT_INTENSITY, Emotion

# --------------------------------------------------------------------------------------
# Totality -- the tests worth having
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize("state", list(TurnState), ids=str)
def test_every_turn_state_produces_a_frame(state: TurnState) -> None:
    frame = frame_for(state)
    assert isinstance(frame.emotion, Emotion)
    assert 0.0 <= frame.intensity <= 1.0
    assert frame.ttl_ms > 0


@pytest.mark.parametrize("emotion", list(Emotion), ids=str)
def test_every_emotion_has_an_accent_colour(emotion: Emotion) -> None:
    """v2.6's five skins read this table. A missing entry is a skin with no element colour."""
    assert ACCENT_COLOR_RE.match(ACCENTS[emotion]), ACCENTS[emotion]


def test_the_accent_colours_are_distinct() -> None:
    """Two emotions sharing a colour would make the halo say less than the face."""
    assert len(set(ACCENTS.values())) == len(ACCENTS)


@pytest.mark.parametrize(
    "table", [BASE_INTENSITY, TTL_MS, STATE_EMOTION], ids=["intensity", "ttl", "emotion"]
)
def test_every_table_covers_every_state(table: dict[TurnState, object]) -> None:
    assert set(table) == set(TurnState)


# --------------------------------------------------------------------------------------
# The documented mapping
# --------------------------------------------------------------------------------------


def test_listening_is_calm() -> None:
    assert frame_for(TurnState.LISTENING).emotion is Emotion.CALM


def test_thinking_is_thinking() -> None:
    assert frame_for(TurnState.THINKING).emotion is Emotion.THINKING


def test_idle_is_neutral() -> None:
    assert frame_for(TurnState.IDLE).emotion is Emotion.NEUTRAL


def test_a_failure_is_the_error_face() -> None:
    assert frame_for(TurnState.FAILED).emotion is Emotion.ERROR


def test_replying_takes_the_models_reported_emotion() -> None:
    """The whole point of the phase."""
    frame = frame_for(TurnState.REPLYING, report=ModelReport(emotion="joy", intensity=0.9))
    assert frame.emotion is Emotion.JOY
    assert frame.intensity == 0.9


def test_replying_without_a_report_is_neutral_not_a_crash() -> None:
    """A model may answer without reporting. Failing on the report rather than on the answer would
    be the tail wagging the dog."""
    assert frame_for(TurnState.REPLYING).emotion is Emotion.NEUTRAL


def test_a_reported_emotion_outside_the_enum_still_produces_a_frame() -> None:
    frame = frame_for(TurnState.REPLYING, report=ModelReport(emotion="ecstatic", intensity="loud"))
    assert frame.emotion is Emotion.NEUTRAL
    assert frame.intensity == DEFAULT_INTENSITY


def test_a_report_outside_replying_is_ignored() -> None:
    """The model has an opinion about its own answer and none about the device listening. Taking
    one would let the previous turn's report colour this turn's `listening` face."""
    for state in (TurnState.IDLE, TurnState.LISTENING, TurnState.THINKING, TurnState.FAILED):
        frame = frame_for(state, report=ModelReport(emotion="joy", intensity=1.0))
        assert frame.emotion is STATE_EMOTION[state]


# --------------------------------------------------------------------------------------
# The fields the mapping fills in
# --------------------------------------------------------------------------------------


def test_the_accent_matches_the_emotion_that_was_chosen_not_the_state() -> None:
    frame = frame_for(TurnState.REPLYING, report=ModelReport(emotion="sad", intensity=0.5))
    assert frame.accent_color == ACCENTS[Emotion.SAD]


def test_gaze_stays_absent_in_this_phase() -> None:
    """Absent means "no opinion". v2.5 (direction) and v3 (the vision turn) are its real sources,
    and a centred gaze invented here would be indistinguishable from a real instruction."""
    for state in TurnState:
        assert frame_for(state).gaze is None


def test_speaking_is_passed_through_and_defaults_off() -> None:
    assert frame_for(TurnState.REPLYING).speaking is False
    assert frame_for(TurnState.REPLYING, speaking=True).speaking is True


def test_listening_outlives_the_longest_utterance() -> None:
    """A ttl shorter than an utterance would relax the face while someone was mid-sentence."""
    from roboface_server.protocol import AUDIO_SAMPLE_RATE, MAX_UTTERANCE_BYTES

    longest_utterance_ms = MAX_UTTERANCE_BYTES / (AUDIO_SAMPLE_RATE * 2) * 1000
    assert TTL_MS[TurnState.LISTENING] > longest_utterance_ms


def test_replying_has_the_longest_ttl() -> None:
    """The device plays audio the server finished sending long ago, so this is the one state whose
    real duration is set by the device rather than by the server (v2.1.2)."""
    assert TTL_MS[TurnState.REPLYING] == max(TTL_MS.values())

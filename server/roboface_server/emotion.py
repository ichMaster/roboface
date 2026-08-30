"""The emotion engine: what the face does, and why.

ARCHITECTURE §2 names this module as *"an emotion engine (decides `EmotionFrame` from the turn and
the model's own reported state)"*. It is the whole of that decision, and it is **pure** — no socket,
no clock, no provider. Given where a turn is and what the model said about itself, it returns one
frame. That is what lets the mapping be read in one screen and tested without a turn.

**Why the server decides at all.** Until v2.2 the firmware chose its own expression from its own
state machine: `recipeFor(DeviceState)`. That worked and was wrong — it put a character decision on
the tier that is meant to be thin, and it meant the model's answer could be cheerful while the face
was merely "replying". MISSION's rule is that all intelligence lives on the server; this module is
the last piece of it to move.

**What the device still decides for itself**, and why that is not a contradiction: `boot`,
`wifi_connecting`, `offline` and a local fault are facts the server cannot know — by definition, in
the case of the ones that mean "the server is unreachable". Those keep a local frame. Everything
inside a turn comes from here.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Final

from roboface_server.protocol import DEFAULT_TTL_MS, Emotion, EmotionFrame, Gaze


class TurnState(StrEnum):
    """Where a turn is, expressed as the five situations that have a face.

    Not the router's `ConnectionPhase` and not the firmware's `DeviceState`: those describe a
    socket and a device, and this describes a *turn*. Keeping it separate is what stops "the
    connection is ready" and "the device is idle" from being conflated with "nothing is happening",
    which are three different things that happen to coincide most of the time.
    """

    IDLE = "idle"
    LISTENING = "listening"
    THINKING = "thinking"
    REPLYING = "replying"
    FAILED = "failed"


@dataclass(frozen=True, slots=True)
class ModelReport:
    """What the model said about its own state -- **before** validation.

    Deliberately typed as `object`: this is untrusted output, and giving the fields their intended
    types here would be a claim that something has already checked them. Nothing has.
    :meth:`EmotionFrame.from_model` is where they become renderable.
    """

    emotion: object
    intensity: object


#: The colour each emotion lends the skin's element -- and, from v5, the halo. One table, decided
#: here rather than per-skin, which is what "a skin is an asset swap" has to mean in practice: five
#: skins expressing joy must agree on *what joy looks like* even when they disagree about
#: everything else.
#:
#: `#rrggbb`, because the device converts to RGB565 and anything else would have to be rejected
#: there. Chosen around the renderer's own ink (a soft cyan-white) so the accents read as the same
#: character in different moods rather than as different characters.
ACCENTS: Final[dict[Emotion, str]] = {
    Emotion.NEUTRAL: "#5FC8FF",    # the resting cyan -- everything else is a departure from it
    Emotion.CALM: "#5FFFC4",       # mint; the colour in ARCHITECTURE's own example
    Emotion.JOY: "#FFD25F",        # warm amber, the only genuinely warm entry
    Emotion.THINKING: "#A88FFF",   # violet: cool, and not on the neutral-to-joy axis at all
    Emotion.SURPRISED: "#FF9F5F",  # orange -- joy's warmth, pushed and sharpened
    Emotion.SAD: "#5F7CFF",        # deep blue; neutral's hue, drained of light
    Emotion.ERROR: "#FF5F6D",      # red, and used for nothing else
}

#: How expressive each state is when the model has no opinion. Not one constant: `listening` should
#: be visibly attentive and `idle` should not compete with the room.
BASE_INTENSITY: Final[dict[TurnState, float]] = {
    TurnState.IDLE: 0.35,
    TurnState.LISTENING: 0.75,
    TurnState.THINKING: 0.60,
    TurnState.REPLYING: 0.60,
    TurnState.FAILED: 0.85,
}

#: How long each frame stands before the device relaxes to `neutral`.
#:
#: **A liveness guarantee, not a schedule.** The server sends a frame on every state change, so in
#: normal operation no ttl ever expires. What these numbers bound is the *abnormal* case: the
#: connection dying between two state changes, leaving the last face standing. So each is "longer
#: than this state can legitimately last", and no longer:
#:
#: * `listening` — a person may hold the floor for `MAX_UTTERANCE_BYTES`, which is 30 seconds. A ttl
#:   shorter than that would relax the face while someone was still mid-sentence, which is the exact
#:   opposite of what the state is for.
#: * `thinking` — a model call plus a retry. Gemini Flash with `thinkingBudget: 0` answers in under
#:   a second; fifteen is the pathological case, not the expected one.
#: * `replying` — the device is playing audio the server finished sending long ago, and a long reply
#:   is a minute of speech. This is the one state where the ttl is genuinely load-bearing, because
#:   the device's playback outlives the server's turn (v2.1.2).
#: * `failed` — an error face should not fade out on its own while the fault is unresolved. It ends
#:   when the next real frame replaces it.
#: * `idle` — the default; a `neutral` frame relaxing to `neutral` is a no-op, so the number here
#:   only ever costs a crossfade that changes nothing.
TTL_MS: Final[dict[TurnState, int]] = {
    TurnState.IDLE: DEFAULT_TTL_MS,
    TurnState.LISTENING: 35_000,
    TurnState.THINKING: 15_000,
    TurnState.REPLYING: 60_000,
    TurnState.FAILED: 20_000,
}

#: What each state looks like when the model has not said otherwise. `replying` is the exception and
#: the point of the phase: it is the only state whose emotion is the model's to choose.
STATE_EMOTION: Final[dict[TurnState, Emotion]] = {
    TurnState.IDLE: Emotion.NEUTRAL,
    TurnState.LISTENING: Emotion.CALM,
    TurnState.THINKING: Emotion.THINKING,
    TurnState.REPLYING: Emotion.NEUTRAL,  # only until a report arrives; see `frame_for`
    TurnState.FAILED: Emotion.ERROR,
}


#: What the character says when it is touched — or, far more often, nothing.
#:
#: **"Nothing" is the first-class outcome and most of the table is it.** A device that is picked up
#: and put down again does not need a line, and a character that remarks on every touch is one
#: nobody wants on a desk. So this maps the *few* events worth a word, and everything absent from it
#: is deliberately silent.
#:
#: The lines are prompts, not scripts: they go to the model as the person's turn would, so the
#: character answers in its own voice rather than reciting. A canned string would be a second
#: personality living in a dictionary.
EVENT_PROMPTS: Final[dict[tuple[str, str], str]] = {
    ("touch", "stroke"): "Тебе щойно погладили по щоці. Скажи щось коротке у відповідь.",
    ("touch", "poke_eye"): "Тебе щойно тицьнули в око. Відреагуй коротко.",
    ("motion", "shake"): "Тебе щойно струснули. Скажи щось коротке.",
    ("motion", "upside_down"): "Тебе перевернули догори дриґом. Скажи щось коротке.",
    ("motion", "free_fall"): "Тебе щойно впустили. Скажи щось дуже коротке.",
}

#: What the face does, which is a **wider** set than what the character says. A tap deserves an
#: expression and not a remark; the device already produced the reflex locally, and this is the
#: server agreeing with it a moment later rather than repeating it.
EVENT_EMOTIONS: Final[dict[tuple[str, str], Emotion]] = {
    ("touch", "tap"): Emotion.JOY,
    ("touch", "multi_tap"): Emotion.JOY,
    ("touch", "stroke"): Emotion.CALM,
    ("touch", "poke_eye"): Emotion.SURPRISED,
    ("motion", "shake"): Emotion.SURPRISED,
    ("motion", "upside_down"): Emotion.SURPRISED,
    ("motion", "free_fall"): Emotion.SAD,
    ("proximity", "approach"): Emotion.CALM,
    # `voice`/`direction` is deliberately absent from both tables: where a person is standing is
    # not a mood and not a remark. It changes where the face *looks*, which is `gaze`, and a
    # character that said something every time someone moved would be unbearable.
}


def reaction_to(event_type: str, kind: str) -> tuple[Emotion | None, str | None]:
    """What the character makes of an event: an expression, a line, or neither.

    Returning a pair rather than an object because the two are genuinely independent -- a tap gets a
    face and no words, a stroke gets both, and being put down gets neither.
    """
    return EVENT_EMOTIONS.get((event_type, kind)), EVENT_PROMPTS.get((event_type, kind))


def frame_for(
    state: TurnState,
    *,
    report: ModelReport | None = None,
    speaking: bool = False,
    gaze: Gaze | None = None,
) -> EmotionFrame:
    """One state, one optional model report -> one frame.

    The mapping, in full: `listening` is `calm`, `thinking` is `thinking`, a failure is `error`, and
    idle is `neutral`. **`replying` is the model's**, falling back to `neutral` when there is no
    report — which happens for real, not only in tests: a model may answer without reporting, and a
    turn that failed on the report rather than on the answer would be the tail wagging the dog.

    A report outside `replying` is ignored rather than refused. The model has an opinion about its
    own answer; it has none about the device listening, and taking one would let a stale report from
    the previous turn colour the next one's `listening` face.

    `speaking` is passed through to the frame as the **permission** it is. What decides when the
    mouth stops is the device's own playback state -- see ARCHITECTURE §EmotionFrame and the v2.1.2
    release notes, where taking the server's word for it froze the mouth mid-reply.
    """
    if state is TurnState.REPLYING and report is not None:
        # The one branch where the model's word is taken -- and it is still coerced. `from_model` is
        # total over arbitrary input, which is what makes "never trusting raw output" a property of
        # the type rather than a habit of the caller.
        frame = EmotionFrame.from_model(report.emotion, report.intensity)
        emotion = frame.emotion
        intensity = frame.intensity
    else:
        emotion = STATE_EMOTION[state]
        intensity = BASE_INTENSITY[state]

    return EmotionFrame(
        emotion=emotion,
        intensity=intensity,
        # **v2.5 gave this a voice, and the comment that stood here predicted it.**
        #
        # `None` still means "no opinion", and that is the common case: the server only has one when
        # the device has told it where a speaker is. Absent is not centre -- the device's own idle
        # drift owns the face when nobody has an opinion, and a centred gaze arriving from here
        # would fight it for control and win, holding the eyes rigidly forward.
        gaze=gaze,
        accent_color=ACCENTS[emotion],
        speaking=speaking,
        ttl_ms=TTL_MS[state],
    )

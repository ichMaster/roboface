"""The device<->server wire contract -- the first of the five seams.

This module is **pure**: no FastAPI, no websockets, no I/O, no clock. That is what lets it
be the single definition both tiers agree on -- the firmware's ``ws_protocol.h`` mirrors it
from v0.3, and a contract test pins it so neither side can drift alone. Anything that needs
a socket belongs in ``router``; anything that needs a decision belongs in the orchestrator.

What it owns (ARCHITECTURE.md §Contracts -> WS device<->server):

* the **message vocabulary** in both directions, declared whole even where a later phase
  implements it -- the contract is the deliverable, not the subset currently in use;
* the eleven-value :class:`ErrorCode` enum -- errors are enumerated, never free text;
* the **JSON text-frame codec**, with typed frames for what v0.1 exchanges and a generic
  envelope for everything else;
* ``hello`` negotiation against :data:`PROTO_VERSION`;
* the **PCM16 / 16 kHz / mono** constants, which are the authority the ASR and TTS adapters
  read their formats from (ARCHITECTURE notes the format is fixed in three places at once
  and they must agree);
* the rule that **binary frames carry raw payloads with no envelope** -- their meaning comes
  from direction plus connection state, expressed here as a predicate rather than left as a
  convention each caller re-invents.

**Whose fault a frame is.** :attr:`ErrorCode.bad_frame` means *the device sent something this
server cannot parse* — malformed JSON, an unknown message type, or one this version does not
implement yet. :attr:`ErrorCode.internal` keeps its real meaning: the server broke. v0.1 had
no code for the first case and reported all of them as ``internal``, which told the device the
server had failed when the device's own frame was at fault — and from v0.4 would render the
error face for a fault the device caused. The twelfth code was added deliberately in v0.2
rather than assumed in v0.1, because changing an enumerated contract is a decision, not a fix.
"""

from __future__ import annotations

import json
import logging
import re
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Any, Final

# --------------------------------------------------------------------------------------
# Version
# --------------------------------------------------------------------------------------

#: The wire version this server speaks. A device announcing anything else is rejected with
#: :attr:`ErrorCode.proto_unsupported` -- there is no compatibility window, by design: one
#: server serves one household's devices and they are flashed together.
PROTO_VERSION: Final = 1


# --------------------------------------------------------------------------------------
# Audio format -- the single authority
# --------------------------------------------------------------------------------------

AUDIO_FORMAT: Final = "pcm16"
AUDIO_SAMPLE_RATE: Final = 16000
AUDIO_CHANNELS: Final = 1

#: The string a device puts in ``hello.audio_fmt``: ``format/rate/channels``. Built from the
#: three constants above so the wire spelling can never drift from them.
AUDIO_FMT: Final = f"{AUDIO_FORMAT}/{AUDIO_SAMPLE_RATE}/{AUDIO_CHANNELS}"


# --------------------------------------------------------------------------------------
# Frame size
# --------------------------------------------------------------------------------------

#: The largest text frame this server will parse. Control frames are small by construction
#: -- the biggest is a ``hello`` with every capability, or a debug-channel ``text_in`` line --
#: so 64 KiB is generous by three orders of magnitude and still bounded.
#:
#: Enforced *before* ``json.loads`` in :func:`decode_envelope`: the point of a cap is not to
#: reject a parsed blob but never to allocate one. The server binds ``0.0.0.0`` by design (the
#: device connects across the LAN) and v0.1 has no authentication, so "only our device talks to
#: it" is a claim about the network, not about this process.
#:
#: The longest utterance the server will assemble, in **bytes rather than seconds** -- it is a
#: property of what arrived, not of a clock, and a clock would have to be trusted across a network.
#: At ``pcm16/16000/1`` this is 30 seconds, which is far longer than anyone holds a button to talk
#: to a desk companion and short enough that a stuck transmitter cannot exhaust the server.
#:
#: Exceeding it ends the window with an enumerated error rather than truncating: someone who talked
#: too long should be told, not silently half-heard.
MAX_UTTERANCE_BYTES: Final = AUDIO_SAMPLE_RATE * 2 * 30

#: Bulk payloads do not pass through here at all -- audio and JPEG are **binary** frames with
#: no envelope, and each will carry its own limit in the phase that introduces it (v1, v3).
#:
#: Note the unit: for a ``str`` this bounds *characters*, and UTF-8 can be up to four bytes
#: each. That is intentional -- the constant exists to stop unbounded growth, not to be an
#: exact byte budget, and checking length is O(1) where encoding first would allocate the very
#: copy the cap is meant to prevent. uvicorn's transport-level ``ws_max_size`` (16 MiB by
#: default) remains the outer backstop.
MAX_TEXT_FRAME_BYTES: Final = 64 * 1024


# --------------------------------------------------------------------------------------
# Message vocabulary
# --------------------------------------------------------------------------------------


#: How long an ``emotion`` frame stands before the device relaxes to ``neutral``, in milliseconds
#: (ARCHITECTURE §EmotionFrame). The device applies this same number when the field is absent, so
#: the two halves agree without the frame having to say so on every state change.
#:
#: It is a **liveness guarantee, not a schedule**: the server sends a frame on every state change,
#: and this is what stops a face holding `thinking` forever when the connection drops between the
#: model call starting and its answer arriving.
DEFAULT_TTL_MS: Final = 8000

#: What an unusable ``intensity`` becomes. The midpoint, because it is the only value that is wrong
#: by the same amount in both directions -- a default of 0 would render as a face that has gone
#: blank, and 1 as one permanently shouting, and both look like a decision rather than a fallback.
DEFAULT_INTENSITY: Final = 0.5

#: ``accent_color`` on the wire: ``#rrggbb``, nothing else. Not a general CSS colour parser -- the
#: device draws with RGB565 and would have to reject anything it could not convert anyway, so the
#: narrow spelling is the honest one.
ACCENT_COLOR_RE: Final = re.compile(r"^#[0-9a-fA-F]{6}$")


class DeviceMessage(StrEnum):
    """Everything a device may send. ``AUDIO`` and ``IMAGE`` are binary; the rest are JSON."""

    HELLO = "hello"
    LISTEN_START = "listen_start"
    AUDIO = "audio"
    LISTEN_STOP = "listen_stop"
    TEXT_IN = "text_in"
    EVENT = "event"
    IMAGE_IN = "image_in"
    IMAGE = "image"
    PING = "ping"


class ServerMessage(StrEnum):
    """Everything a server may send. ``TTS_AUDIO`` is binary; the rest are JSON."""

    ASR_PARTIAL = "asr_partial"
    ASR = "asr"
    REPLY = "reply"
    EMOTION = "emotion"
    TTS_AUDIO = "tts_audio"
    TTS_END = "tts_end"
    CONFIG_UPDATED = "config_updated"
    ERROR = "error"
    RESTART = "restart"
    PONG = "pong"


#: Binary frames carry a raw payload and **no JSON envelope**, so they never pass through the
#: text codec. Keeping the membership here rather than in the codec is what makes the rule
#: checkable from either side.
BINARY_DEVICE_MESSAGES: Final = frozenset({DeviceMessage.AUDIO, DeviceMessage.IMAGE})
BINARY_SERVER_MESSAGES: Final = frozenset({ServerMessage.TTS_AUDIO})

TEXT_DEVICE_MESSAGES: Final = frozenset(DeviceMessage) - BINARY_DEVICE_MESSAGES
TEXT_SERVER_MESSAGES: Final = frozenset(ServerMessage) - BINARY_SERVER_MESSAGES


class Emotion(StrEnum):
    """The fixed face vocabulary (ARCHITECTURE §EmotionFrame).

    **Seven values, and adding an eighth is a contract change, not a feature.** The device's recipe
    table must be total over this enum -- every value has a face, drawn from arithmetic in
    `firmware/src/pure/face.h`. A value the firmware has no recipe for does not raise anywhere; it
    renders as a blank face, which is the worst place to discover a missing case and the reason the
    enum is closed rather than free-form.
    """

    NEUTRAL = "neutral"
    CALM = "calm"
    JOY = "joy"
    THINKING = "thinking"
    SURPRISED = "surprised"
    SAD = "sad"
    ERROR = "error"


class EventType(StrEnum):
    """What kind of thing happened to the device (ARCHITECTURE §event{})."""

    TOUCH = "touch"
    MOTION = "motion"
    PROXIMITY = "proximity"


#: The `kind` vocabulary, per type. ARCHITECTURE §event{} lists exactly these, and the firmware's
#: `ws_protocol.h` mirrors them -- `tests/contract/test_firmware_mirror.py` checks the two agree,
#: because a value one side has never heard of is a reaction the character silently never gives.
EVENT_KINDS: Final[dict[EventType, frozenset[str]]] = {
    EventType.TOUCH: frozenset({"tap", "multi_tap", "stroke", "poke_eye", "long_press"}),
    EventType.MOTION: frozenset({"tilt", "shake", "picked_up", "upside_down", "free_fall"}),
    EventType.PROXIMITY: frozenset({"approach", "leave"}),
}

#: The largest `meta` an event may carry, in **entries**. `meta` is free-form by design -- a touch
#: reports a zone and a count, a motion reports an axis, and pinning that would mean a protocol
#: change per sensor. Free-form is not unbounded, though: the device is trusted to be ours and the
#: network is not, and a cap that is never reached costs nothing.
MAX_EVENT_META_ENTRIES: Final = 8


class ErrorCode(StrEnum):
    """The enumerated failure vocabulary. Never free text -- the device renders a face per code."""

    WIFI_LOST = "wifi_lost"
    SERVER_UNREACHABLE = "server_unreachable"
    PROTO_UNSUPPORTED = "proto_unsupported"
    UNAUTHORIZED = "unauthorized"
    RATE_LIMITED = "rate_limited"
    ASR_FAILED = "asr_failed"
    LLM_TIMEOUT = "llm_timeout"
    LLM_FAILED = "llm_failed"
    TTS_FAILED = "tts_failed"
    VISION_FAILED = "vision_failed"
    BAD_FRAME = "bad_frame"
    INTERNAL = "internal"


class Capability(StrEnum):
    """``hello.caps`` -- what the board physically has (ARCHITECTURE §Hardware variants)."""

    TOUCH = "touch"
    CAMERA = "camera"
    DUAL_MIC = "dual_mic"
    HALO = "halo"
    BUTTONS = "buttons"


# --------------------------------------------------------------------------------------
# Binary-frame meaning: direction + connection state, never an envelope
# --------------------------------------------------------------------------------------


class BinaryPhase(StrEnum):
    """What a connection is doing, which is what gives an unlabelled binary frame meaning."""

    IDLE = "idle"
    LISTENING = "listening"
    AWAITING_IMAGE = "awaiting_image"
    SPEAKING = "speaking"


def device_binary_meaning(phase: BinaryPhase) -> DeviceMessage | None:
    """What a binary frame *from* a device means in ``phase``; ``None`` if it means nothing.

    ``None`` is the interesting answer: a binary frame arriving outside a listening window or
    an announced image is not a frame with a default meaning, it is a protocol violation.
    """
    return {
        BinaryPhase.LISTENING: DeviceMessage.AUDIO,
        BinaryPhase.AWAITING_IMAGE: DeviceMessage.IMAGE,
    }.get(phase)


def server_binary_meaning(phase: BinaryPhase) -> ServerMessage | None:
    """What a binary frame *to* a device means in ``phase``; ``None`` if it means nothing."""
    return ServerMessage.TTS_AUDIO if phase is BinaryPhase.SPEAKING else None


# --------------------------------------------------------------------------------------
# Errors raised by the codec
# --------------------------------------------------------------------------------------


class ProtocolError(Exception):
    """Base class for every codec failure. Always carries an enumerated code.

    Defaults to :attr:`ErrorCode.BAD_FRAME`, not ``INTERNAL``: everything raised from this
    codec is a judgement about what *arrived*, and attributing that to the server would send
    the device the wrong face and the wrong retry behaviour.
    """

    def __init__(self, message: str, code: ErrorCode = ErrorCode.BAD_FRAME) -> None:
        super().__init__(message)
        self.code = code


class MalformedFrame(ProtocolError):
    """Not JSON, not an object, no ``type``, or a required field missing or wrong-typed."""


class UnknownMessage(ProtocolError):
    """A ``type`` that is not in either direction's vocabulary."""


class UnsupportedMessage(ProtocolError):
    """A declared message type whose payload this phase does not decode yet.

    Distinct from :class:`UnknownMessage` on purpose: the router answers this with a clean
    enumerated ``error`` rather than treating the device as broken, and each later phase
    turns one of these into a typed frame as it implements it.
    """

    def __init__(self, message_type: DeviceMessage | ServerMessage) -> None:
        super().__init__(f"message type {message_type.value!r} is declared but not implemented")
        self.message_type = message_type


# --------------------------------------------------------------------------------------
# Typed frames -- the ones v0.1 exchanges
# --------------------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Hello:
    """``hello{device_id, proto_ver, audio_fmt, caps}`` -- always the first frame."""

    device_id: str
    proto_ver: int
    audio_fmt: str
    caps: frozenset[Capability]


@dataclass(frozen=True, slots=True)
class TextIn:
    """``text_in{text}`` -- a typed line, the v0 turn source (serial debug channel)."""

    text: str


@dataclass(frozen=True, slots=True)
class Ping:
    """``ping`` -- liveness, device -> server."""


@dataclass(frozen=True, slots=True)
class Pong:
    """``pong`` -- liveness, server -> device."""


@dataclass(frozen=True, slots=True)
class Reply:
    """``reply{text, final}``.

    One delta per frame from v0.2: the reply is streamed, never accumulated and sent whole.
    ``final`` marks the last frame of a turn, not the only one.
    """

    text: str
    final: bool


@dataclass(frozen=True, slots=True)
class ListenStart:
    """``listen_start`` -- the device opens an utterance, device -> server.

    Carries nothing. The binary frames that follow are `audio` **because the connection is
    listening** (``device_binary_meaning``), not because anything labels them, so this frame's whole
    job is to open that window.
    """


@dataclass(frozen=True, slots=True)
class ListenStop:
    """``listen_stop`` -- the utterance is over, device -> server."""


@dataclass(frozen=True, slots=True)
class AsrPartial:
    """``asr_partial{text}`` -- what the server currently believes it heard, server -> device.

    Revised constantly and never authoritative. The device shows it as a guess; it is `asr` that
    settles what was said.
    """

    text: str


@dataclass(frozen=True, slots=True)
class Asr:
    """``asr{text}`` -- the resolved utterance, server -> device."""

    text: str


@dataclass(frozen=True, slots=True)
class TtsEnd:
    """``tts_end`` -- the speaking window is closed, server -> device.

    Carries nothing. The binary frames that preceded it were `tts_audio` **because the connection
    was speaking**, not because anything labelled them (``server_binary_meaning``), so this frame's
    whole job is to end that phase. A device that has drained its buffer on seeing this can switch
    the shared I2S bus back to the microphone.
    """


@dataclass(frozen=True, slots=True)
class Event:
    """``event{type, kind, meta}`` -- the second level of the reaction model, device -> server.

    The reflex has already fired locally by the time this is sent; this tells the *character* it
    happened, and the server may answer with an emotion, a spoken line, or nothing at all.

    **An unknown `kind` is refused, which is the opposite of what `emotion{}` does.** The contrast
    is deliberate. A face is not worth dropping a connection over, so a bad emotion is coerced to
    `neutral` and rendered. But an event is the *device making a claim* about the physical world,
    and a claim the contract does not define is a firmware bug: coercing it would turn that bug
    into a wrong reaction from the character, which is far harder to trace than a refused frame.
    """

    type: EventType
    kind: str
    meta: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True, slots=True)
class Gaze:
    """Where the face is looking: ``x`` and ``y`` in −1..1, centre at the origin.

    Its own type rather than a pair of floats on the frame, because gaze acquires real sources
    later -- inter-microphone direction in v2.5, the vision turn in v3 -- and a reflex on the device
    overrides it. Naming it now is what keeps those three from each inventing a shape.
    """

    x: float = 0.0
    y: float = 0.0


@dataclass(frozen=True, slots=True)
class EmotionFrame:
    """``emotion{...}`` -- the entire face channel, server -> device, one object.

    ``emotion`` and ``intensity`` are required; the rest carry the documented defaults, and
    :func:`encode` omits any field still at its default. That is not compression -- a frame goes out
    on every state change, and a wire full of ``"gaze":{"x":0,"y":0}`` makes the fields that *did*
    change harder to see in a log.

    **The audio level is deliberately not here.** Lip-sync is local, from the playback buffer the
    device is already holding (ARCHITECTURE §EmotionFrame). Putting a level on this frame would put
    a 30-per-second signal on a channel that otherwise carries a handful of frames per turn.
    """

    emotion: Emotion
    intensity: float
    gaze: Gaze | None = None
    accent_color: str | None = None
    speaking: bool = False
    ttl_ms: int = DEFAULT_TTL_MS

    @classmethod
    def from_model(
        cls,
        emotion: object,
        intensity: object,
        *,
        gaze: object = None,
        accent_color: object = None,
        speaking: bool = False,
        ttl_ms: int = DEFAULT_TTL_MS,
    ) -> EmotionFrame:
        """Build a frame from **untrusted** model output, coercing rather than raising.

        The model is asked for an emotion from the enum. It is not obliged to comply, and the one
        thing that must never happen is a turn failing -- or a face going blank -- because a
        language model spelled ``joy`` as ``happy``. So every field has a documented coercion and
        this method is total over arbitrary input.

        Each coercion logs once at debug with what it saw. A model that keeps reporting ``happy`` is
        a **prompt** bug, not a protocol one, and the only way to find it is for the coercion to
        leave a trace rather than quietly succeed.
        """
        return cls(
            emotion=coerce_emotion(emotion),
            intensity=coerce_intensity(intensity),
            gaze=coerce_gaze(gaze),
            accent_color=coerce_accent_color(accent_color),
            speaking=speaking,
            ttl_ms=ttl_ms,
        )


@dataclass(frozen=True, slots=True)
class ErrorFrame:
    """``error{code, msg}`` -- ``msg`` is for a human reading a log; ``code`` is the contract."""

    code: ErrorCode
    msg: str


#: Everything :func:`decode` can return and :func:`encode` accepts.
Frame = (
    Hello | TextIn | ListenStart | ListenStop | Ping | Pong | Event
    | AsrPartial | Asr | Reply | EmotionFrame | TtsEnd | ErrorFrame
)

#: Which message type each typed frame is. One table, so encode and the tests agree.
FRAME_TYPES: Final[dict[type[Frame], DeviceMessage | ServerMessage]] = {
    Hello: DeviceMessage.HELLO,
    Event: DeviceMessage.EVENT,
    TextIn: DeviceMessage.TEXT_IN,
    ListenStart: DeviceMessage.LISTEN_START,
    ListenStop: DeviceMessage.LISTEN_STOP,
    Ping: DeviceMessage.PING,
    Pong: ServerMessage.PONG,
    AsrPartial: ServerMessage.ASR_PARTIAL,
    Asr: ServerMessage.ASR,
    Reply: ServerMessage.REPLY,
    EmotionFrame: ServerMessage.EMOTION,
    TtsEnd: ServerMessage.TTS_END,
    ErrorFrame: ServerMessage.ERROR,
}


# --------------------------------------------------------------------------------------
# Codec
# --------------------------------------------------------------------------------------


def encode(frame: Frame) -> str:
    """Serialize a typed frame to its JSON text-frame payload."""
    message_type = FRAME_TYPES[type(frame)]
    payload: dict[str, Any] = {"type": str(message_type)}

    match frame:
        case Hello():
            payload |= {
                "device_id": frame.device_id,
                "proto_ver": frame.proto_ver,
                "audio_fmt": frame.audio_fmt,
                "caps": sorted(str(cap) for cap in frame.caps),
            }
        case TextIn():
            payload["text"] = frame.text
        case Event():
            # **Nested under `event`, not spread into the envelope.** ARCHITECTURE §event{} shows
            # `{"type": "touch", "kind": …}` and that is the *inner* object: the envelope's own
            # `type` names the message, a rule every other frame follows and `decode_envelope`
            # depends on. Spreading them collides -- the event's type overwrites the message's, and
            # the frame decodes as an unknown message called "touch".
            inner: dict[str, Any] = {"type": str(frame.type), "kind": frame.kind}
            if frame.meta:
                inner["meta"] = dict(frame.meta)
            payload["event"] = inner
        case AsrPartial() | Asr():
            payload["text"] = frame.text
        case Reply():
            payload |= {"text": frame.text, "final": frame.final}
        case EmotionFrame():
            # Required always; optional only when it says something. A frame goes out on every
            # state change, so a wire full of defaults would bury the field that actually moved.
            payload |= {"emotion": str(frame.emotion), "intensity": frame.intensity}
            if frame.gaze is not None:
                payload["gaze"] = {"x": frame.gaze.x, "y": frame.gaze.y}
            if frame.accent_color is not None:
                payload["accent_color"] = frame.accent_color
            if frame.speaking:
                payload["speaking"] = True
            if frame.ttl_ms != DEFAULT_TTL_MS:
                payload["ttl_ms"] = frame.ttl_ms
        case ErrorFrame():
            payload |= {"code": str(frame.code), "msg": frame.msg}
        case Ping() | Pong() | TtsEnd() | ListenStart() | ListenStop():
            pass

    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def decode_envelope(raw: str | bytes) -> tuple[str, dict[str, Any]]:
    """Parse a text frame down to ``(type, payload)`` without interpreting the payload.

    The generic layer: it round-trips any declared message, including the ones no phase
    implements yet, which is what lets a later phase add a typed frame without touching this.

    Oversize frames are refused here, before any parsing happens.
    """
    if len(raw) > MAX_TEXT_FRAME_BYTES:
        raise MalformedFrame(
            f"text frame is {len(raw)} long; the limit is {MAX_TEXT_FRAME_BYTES}"
        )

    try:
        parsed = json.loads(raw)
    except (TypeError, ValueError) as exc:
        raise MalformedFrame(f"frame is not valid JSON: {exc}") from exc

    if not isinstance(parsed, dict):
        raise MalformedFrame(f"frame must be a JSON object, got {type(parsed).__name__}")

    message_type = parsed.get("type")
    if message_type is None:
        raise MalformedFrame("frame has no 'type'")
    if not isinstance(message_type, str):
        raise MalformedFrame(f"'type' must be a string, got {type(message_type).__name__}")

    return message_type, parsed


def message_type_of(name: str) -> DeviceMessage | ServerMessage | None:
    """The declared message type called ``name``, or ``None``.

    The two vocabularies are disjoint, so one lookup is unambiguous and a single ``decode``
    serves both tiers -- the fake device uses the very same codec the server does.
    """
    for vocabulary in (DeviceMessage, ServerMessage):
        try:
            return vocabulary(name)
        except ValueError:
            continue
    return None


def decode(raw: str | bytes) -> Frame:
    """Parse a JSON text frame into a typed frame.

    Raises :class:`MalformedFrame` for junk, :class:`UnknownMessage` for a type outside the
    vocabulary, and :class:`UnsupportedMessage` for a declared type this phase has no typed
    frame for yet.
    """
    name, payload = decode_envelope(raw)

    message_type = message_type_of(name)
    if message_type is None:
        raise UnknownMessage(f"unknown message type {name!r}")

    if message_type in BINARY_DEVICE_MESSAGES or message_type in BINARY_SERVER_MESSAGES:
        raise MalformedFrame(
            f"{name!r} is a binary frame and carries no JSON envelope; "
            "its meaning comes from direction plus connection state"
        )

    decoder = _DECODERS.get(message_type)
    if decoder is None:
        raise UnsupportedMessage(message_type)
    return decoder(payload)


# --------------------------------------------------------------------------------------
# Coercions -- what makes untrusted model output safe to render
# --------------------------------------------------------------------------------------

#: The one logger the server emits through (``logging.LOGGER_NAME``). Named literally rather than
#: imported, because ``roboface_server.logging`` shadows the stdlib module inside this package and
#: `protocol` is the module both tiers read as the definition of the contract -- it must not acquire
#: an import that could fail differently depending on who imports it first.
_LOG: Final = logging.getLogger("roboface")


def coerce_emotion(value: object) -> Emotion:
    """Anything at all -> a renderable emotion. Unknown or absent becomes ``neutral``."""
    if isinstance(value, Emotion):
        return value
    if isinstance(value, str):
        try:
            return Emotion(value)
        except ValueError:
            _LOG.debug("model reported an emotion outside the enum: %r -> neutral", value)
            return Emotion.NEUTRAL
    _LOG.debug("model reported a non-string emotion: %r -> neutral", value)
    return Emotion.NEUTRAL


def coerce_intensity(value: object) -> float:
    """Anything at all -> 0..1.

    ``bool`` is excluded explicitly: it is an ``int`` subclass, so ``True`` would otherwise arrive
    as full intensity rather than as the malformed value it is.
    """
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _LOG.debug("model reported a non-numeric intensity: %r -> %s", value, DEFAULT_INTENSITY)
        return DEFAULT_INTENSITY
    number = float(value)
    # NaN fails every comparison, so it must be caught by asking rather than by clamping.
    if number != number:
        _LOG.debug("model reported NaN intensity -> %s", DEFAULT_INTENSITY)
        return DEFAULT_INTENSITY
    if number < 0.0 or number > 1.0:
        _LOG.debug("model reported intensity %r, clamped", number)
    return min(1.0, max(0.0, number))


def _coerce_axis(value: object, axis: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0.0
    number = float(value)
    if number != number:
        return 0.0
    if number < -1.0 or number > 1.0:
        _LOG.debug("gaze %s was %r, clamped", axis, number)
    return min(1.0, max(-1.0, number))


def coerce_gaze(value: object) -> Gaze | None:
    """A mapping with ``x``/``y`` -> a clamped :class:`Gaze`; anything else -> ``None``.

    ``None`` rather than a centred gaze: "no opinion" and "look straight ahead" are different
    instructions once a reflex on the device can override one of them, and this phase's server has
    no opinion at all.
    """
    if value is None:
        return None
    if not isinstance(value, dict):
        _LOG.debug("gaze was not an object: %r -> omitted", value)
        return None
    return Gaze(x=_coerce_axis(value.get("x"), "x"), y=_coerce_axis(value.get("y"), "y"))


def coerce_accent_color(value: object) -> str | None:
    """``#rrggbb`` -> itself; anything else -> ``None``.

    Dropped rather than defaulted. An accent colour that is absent means "use the recipe's own
    colour", which is a working face; a colour invented here would be a decision the skin never
    agreed to.
    """
    if value is None:
        return None
    if not isinstance(value, str) or ACCENT_COLOR_RE.match(value) is None:
        _LOG.debug("accent_color %r is not #rrggbb -> omitted", value)
        return None
    return value


def _require_str(payload: dict[str, Any], key: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str):
        raise MalformedFrame(f"'{key}' must be a string, got {type(value).__name__}")
    return value


def _require_int(payload: dict[str, Any], key: str) -> int:
    value = payload.get(key)
    # bool is an int subclass in Python; `proto_ver: true` is a malformed frame, not version 1.
    if isinstance(value, bool) or not isinstance(value, int):
        raise MalformedFrame(f"'{key}' must be an integer, got {type(value).__name__}")
    return value


def _require_bool(payload: dict[str, Any], key: str) -> bool:
    value = payload.get(key)
    if not isinstance(value, bool):
        raise MalformedFrame(f"'{key}' must be a boolean, got {type(value).__name__}")
    return value


def parse_caps(value: object) -> frozenset[Capability]:
    """``hello.caps`` -> the known capabilities in it.

    **Unknown entries are ignored, not rejected.** A newer board announcing a capability this
    server has never heard of must still connect -- it simply gets served what it does have.
    A non-list ``caps`` is malformed, though: a bare string would otherwise iterate character
    by character and silently parse as nothing.
    """
    if not isinstance(value, list):
        raise MalformedFrame(f"'caps' must be a list, got {type(value).__name__}")

    known: set[Capability] = set()
    for entry in value:
        if not isinstance(entry, str):
            continue
        try:
            known.add(Capability(entry))
        except ValueError:
            continue
    return frozenset(known)


def _decode_hello(payload: dict[str, Any]) -> Hello:
    return Hello(
        device_id=_require_str(payload, "device_id"),
        proto_ver=_require_int(payload, "proto_ver"),
        audio_fmt=_require_str(payload, "audio_fmt"),
        caps=parse_caps(payload.get("caps", [])),
    )


def _decode_text_in(payload: dict[str, Any]) -> TextIn:
    return TextIn(text=_require_str(payload, "text"))


def _decode_reply(payload: dict[str, Any]) -> Reply:
    return Reply(text=_require_str(payload, "text"), final=_require_bool(payload, "final"))


def _decode_event(payload: dict[str, Any]) -> Event:
    """Decode ``event{}`` -- **validating, not coercing**, which is the opposite of `emotion{}`.

    Both decisions are right and the contrast is the content of each. A face is not worth dropping a
    connection over, so a bad emotion becomes `neutral` and is rendered. An event is the device
    making a *claim about the physical world*; a claim the contract does not define is a firmware
    bug, and coercing it would turn that bug into a wrong reaction from the character -- far harder
    to trace than a refused frame, and arriving as "why did it do that" rather than as an error.
    """
    inner = payload.get("event")
    if not isinstance(inner, dict):
        raise MalformedFrame("'event' must be an object")

    raw_type = _require_str(inner, "type")
    try:
        event_type = EventType(raw_type)
    except ValueError as exc:
        raise MalformedFrame(f"unknown event type {raw_type!r}") from exc

    kind = _require_str(inner, "kind")
    if kind not in EVENT_KINDS[event_type]:
        raise MalformedFrame(f"{kind!r} is not a {raw_type} event")

    meta = inner.get("meta", {})
    if not isinstance(meta, dict):
        raise MalformedFrame(f"'meta' must be an object, got {type(meta).__name__}")
    if len(meta) > MAX_EVENT_META_ENTRIES:
        raise MalformedFrame(
            f"'meta' has {len(meta)} entries; the limit is {MAX_EVENT_META_ENTRIES}"
        )

    return Event(type=event_type, kind=kind, meta=dict(meta))


def _decode_emotion(payload: dict[str, Any]) -> EmotionFrame:
    """Decode ``emotion{}`` -- **coercing, not validating**.

    Unlike every other decoder here, this one never raises on a bad field. It is the same rule the
    firmware's parser applies to the same frame, and it is deliberate on both sides: a face is not
    worth dropping a connection over, and the two halves are separately releasable, so neither may
    assume the other has already sanitised what it sends.

    The frame's *envelope* is still the contract -- a payload that is not an object never reaches
    here at all.
    """
    return EmotionFrame.from_model(
        payload.get("emotion"),
        payload.get("intensity"),
        gaze=payload.get("gaze"),
        accent_color=payload.get("accent_color"),
        speaking=payload.get("speaking") is True,
        ttl_ms=_optional_ttl(payload.get("ttl_ms")),
    )


def _optional_ttl(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        return DEFAULT_TTL_MS
    return value


def _decode_error(payload: dict[str, Any]) -> ErrorFrame:
    raw_code = _require_str(payload, "code")
    try:
        code = ErrorCode(raw_code)
    except ValueError as exc:
        raise MalformedFrame(f"unknown error code {raw_code!r}") from exc
    return ErrorFrame(code=code, msg=_require_str(payload, "msg"))


#: Typed rather than ``Any``: the annotation is what makes "a decoder returns a Frame" a
#: checked promise, so adding one for a later phase cannot quietly widen `decode`.
_Decoder = Callable[[dict[str, Any]], Frame]

_DECODERS: Final[dict[DeviceMessage | ServerMessage, _Decoder]] = {
    DeviceMessage.HELLO: _decode_hello,
    DeviceMessage.TEXT_IN: _decode_text_in,
    DeviceMessage.EVENT: _decode_event,
    DeviceMessage.LISTEN_START: lambda _payload: ListenStart(),
    DeviceMessage.LISTEN_STOP: lambda _payload: ListenStop(),
    DeviceMessage.PING: lambda _payload: Ping(),
    ServerMessage.PONG: lambda _payload: Pong(),
    ServerMessage.ASR_PARTIAL: lambda payload: AsrPartial(text=_require_str(payload, "text")),
    ServerMessage.ASR: lambda payload: Asr(text=_require_str(payload, "text")),
    ServerMessage.REPLY: _decode_reply,
    ServerMessage.EMOTION: _decode_emotion,
    ServerMessage.TTS_END: lambda _payload: TtsEnd(),
    ServerMessage.ERROR: _decode_error,
}


# --------------------------------------------------------------------------------------
# hello negotiation
# --------------------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Accepted:
    """The device speaks this server's protocol; the connection may proceed."""

    hello: Hello


@dataclass(frozen=True, slots=True)
class Rejected:
    """The device does not; the server sends ``error{code}`` and closes."""

    code: ErrorCode
    reason: str


Negotiation = Accepted | Rejected


def negotiate(hello: Hello) -> Negotiation:
    """Decide whether a greeted device may proceed.

    Version only. ``audio_fmt`` is carried through untouched: no audio flows until v1, and
    rejecting on a format this phase never reads would be a rule with nothing behind it.
    """
    if hello.proto_ver != PROTO_VERSION:
        return Rejected(
            code=ErrorCode.PROTO_UNSUPPORTED,
            reason=f"server speaks proto_ver {PROTO_VERSION}, device announced {hello.proto_ver}",
        )
    return Accepted(hello=hello)


def parse_audio_fmt(value: str) -> tuple[str, int, int] | None:
    """``"pcm16/16000/1"`` -> ``("pcm16", 16000, 1)``; ``None`` if it is not that shape.

    Unused in v0.1 -- no audio flows yet. It lives here because the format string is this
    module's to define, and v1 must not re-derive it next to the microphone code.
    """
    parts = value.split("/")
    if len(parts) != 3:
        return None
    encoding, rate, channels = parts
    if not rate.isdigit() or not channels.isdigit():
        return None
    return encoding, int(rate), int(channels)

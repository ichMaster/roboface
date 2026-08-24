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
from collections.abc import Callable
from dataclasses import dataclass
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
class TtsEnd:
    """``tts_end`` -- the speaking window is closed, server -> device.

    Carries nothing. The binary frames that preceded it were `tts_audio` **because the connection
    was speaking**, not because anything labelled them (``server_binary_meaning``), so this frame's
    whole job is to end that phase. A device that has drained its buffer on seeing this can switch
    the shared I2S bus back to the microphone.
    """


@dataclass(frozen=True, slots=True)
class ErrorFrame:
    """``error{code, msg}`` -- ``msg`` is for a human reading a log; ``code`` is the contract."""

    code: ErrorCode
    msg: str


#: Everything :func:`decode` can return and :func:`encode` accepts.
Frame = Hello | TextIn | Ping | Pong | Reply | TtsEnd | ErrorFrame

#: Which message type each typed frame is. One table, so encode and the tests agree.
FRAME_TYPES: Final[dict[type[Frame], DeviceMessage | ServerMessage]] = {
    Hello: DeviceMessage.HELLO,
    TextIn: DeviceMessage.TEXT_IN,
    Ping: DeviceMessage.PING,
    Pong: ServerMessage.PONG,
    Reply: ServerMessage.REPLY,
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
        case Reply():
            payload |= {"text": frame.text, "final": frame.final}
        case ErrorFrame():
            payload |= {"code": str(frame.code), "msg": frame.msg}
        case Ping() | Pong() | TtsEnd():
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
    DeviceMessage.PING: lambda _payload: Ping(),
    ServerMessage.PONG: lambda _payload: Pong(),
    ServerMessage.REPLY: _decode_reply,
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

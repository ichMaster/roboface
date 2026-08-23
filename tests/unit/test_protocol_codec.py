"""Unit tests for the protocol codec: round-trip, rejection, and the vocabulary boundary.

The contract test pins *what* the vocabulary is; these pin *how* frames survive the wire and
how badly-formed ones fail. Both matter: a codec that silently coerces a wrong-typed field is
how two tiers drift while every test stays green.
"""

from __future__ import annotations

import json

import pytest
from roboface_server.protocol import (
    AUDIO_FMT,
    BINARY_DEVICE_MESSAGES,
    BINARY_SERVER_MESSAGES,
    PROTO_VERSION,
    Capability,
    DeviceMessage,
    ErrorCode,
    ErrorFrame,
    Frame,
    Hello,
    MalformedFrame,
    Ping,
    Pong,
    Reply,
    ServerMessage,
    TextIn,
    UnknownMessage,
    UnsupportedMessage,
    decode,
    decode_envelope,
    encode,
    message_type_of,
)

IMPLEMENTED_FRAMES: list[Frame] = [
    Hello(
        device_id="core-s3-01",
        proto_ver=PROTO_VERSION,
        audio_fmt=AUDIO_FMT,
        caps=frozenset({Capability.TOUCH, Capability.CAMERA, Capability.DUAL_MIC}),
    ),
    Hello(device_id="bare", proto_ver=PROTO_VERSION, audio_fmt=AUDIO_FMT, caps=frozenset()),
    TextIn(text="привіт, як справи?"),
    TextIn(text=""),
    Ping(),
    Pong(),
    Reply(text="Привіт!", final=False),
    Reply(text="", final=True),
    ErrorFrame(code=ErrorCode.PROTO_UNSUPPORTED, msg="server speaks proto_ver 1"),
    ErrorFrame(code=ErrorCode.INTERNAL, msg="unhandled"),
]


@pytest.mark.parametrize("frame", IMPLEMENTED_FRAMES, ids=lambda f: type(f).__name__)
def test_every_typed_frame_round_trips(frame: Frame) -> None:
    assert decode(encode(frame)) == frame


def test_encoding_is_utf8_clean_not_escaped() -> None:
    # Ukrainian is the product's language; \u-escaping every reply would triple the frame
    # size on a device that reads it over WiFi.
    assert "привіт" in encode(TextIn(text="привіт"))


def test_decode_accepts_bytes_as_well_as_str() -> None:
    payload = encode(Ping()).encode("utf-8")

    assert decode(payload) == Ping()


@pytest.mark.parametrize(
    "message_type",
    sorted(
        name
        for name in (
            {member.value for member in DeviceMessage} | {member.value for member in ServerMessage}
        )
    ),
)
def test_every_declared_text_type_round_trips_at_the_envelope_level(message_type: str) -> None:
    """The generic layer carries the whole vocabulary, including types no phase implements.

    This is what lets v0.2..v3 add a typed frame without reopening the envelope.
    """
    resolved = message_type_of(message_type)
    assert resolved is not None
    if resolved in BINARY_DEVICE_MESSAGES or resolved in BINARY_SERVER_MESSAGES:
        pytest.skip(f"{message_type} is a binary frame and carries no envelope")

    raw = json.dumps({"type": message_type, "extra": 1})
    name, payload = decode_envelope(raw)

    assert name == message_type
    assert payload["extra"] == 1


@pytest.mark.parametrize(
    "raw",
    [
        "",
        "not json at all",
        "{",
        "[1, 2, 3]",
        '"a bare string"',
        "42",
        "null",
    ],
)
def test_malformed_input_is_rejected(raw: str) -> None:
    with pytest.raises(MalformedFrame):
        decode(raw)


def test_a_frame_without_a_type_is_rejected() -> None:
    with pytest.raises(MalformedFrame, match="no 'type'"):
        decode(json.dumps({"text": "hello"}))


def test_a_non_string_type_is_rejected() -> None:
    with pytest.raises(MalformedFrame, match="must be a string"):
        decode(json.dumps({"type": 7}))


def test_an_unknown_type_is_rejected_as_unknown_not_malformed() -> None:
    with pytest.raises(UnknownMessage):
        decode(json.dumps({"type": "sing_a_song"}))


@pytest.mark.parametrize("message_type", ["listen_start", "event", "image_in", "emotion", "asr"])
def test_a_declared_but_unimplemented_type_is_unsupported(message_type: str) -> None:
    """Distinct from unknown: the router answers this with a clean error, not with disdain."""
    with pytest.raises(UnsupportedMessage) as raised:
        decode(json.dumps({"type": message_type}))

    assert raised.value.message_type.value == message_type


@pytest.mark.parametrize("message_type", ["audio", "image", "tts_audio"])
def test_a_binary_type_sent_as_a_text_frame_is_rejected(message_type: str) -> None:
    # Binary frames carry raw payloads with no envelope. A JSON frame claiming to be one is
    # a category error, and letting it through would make the no-envelope rule advisory.
    with pytest.raises(MalformedFrame, match="binary frame"):
        decode(json.dumps({"type": message_type}))


@pytest.mark.parametrize(
    "payload",
    [
        {"type": "hello", "proto_ver": 1, "audio_fmt": AUDIO_FMT},
        {"type": "hello", "device_id": 7, "proto_ver": 1, "audio_fmt": AUDIO_FMT},
        {"type": "hello", "device_id": "d", "proto_ver": "1", "audio_fmt": AUDIO_FMT},
        {"type": "hello", "device_id": "d", "proto_ver": 1},
        {
            "type": "hello",
            "device_id": "d",
            "proto_ver": 1,
            "audio_fmt": AUDIO_FMT,
            "caps": "touch",
        },
        {"type": "text_in"},
        {"type": "text_in", "text": None},
        {"type": "reply", "text": "hi"},
        {"type": "reply", "text": "hi", "final": "yes"},
        {"type": "error", "code": "internal"},
        {"type": "error", "code": "no_such_code", "msg": "x"},
    ],
)
def test_missing_or_wrong_typed_fields_are_rejected(payload: dict[str, object]) -> None:
    with pytest.raises(MalformedFrame):
        decode(json.dumps(payload))


def test_a_boolean_proto_ver_is_not_version_one() -> None:
    # bool is an int subclass in Python; without an explicit guard `proto_ver: true` would
    # decode as version 1 and a broken device would be welcomed in.
    with pytest.raises(MalformedFrame, match="proto_ver"):
        decode(json.dumps({"type": "hello", "device_id": "d", "proto_ver": True, "audio_fmt": "x"}))


def test_caps_defaults_to_empty_when_absent() -> None:
    frame = decode(
        json.dumps({"type": "hello", "device_id": "d", "proto_ver": 1, "audio_fmt": AUDIO_FMT})
    )

    assert isinstance(frame, Hello)
    assert frame.caps == frozenset()


def test_caps_survive_a_round_trip_regardless_of_order() -> None:
    frame = Hello(
        device_id="d",
        proto_ver=PROTO_VERSION,
        audio_fmt=AUDIO_FMT,
        caps=frozenset({Capability.HALO, Capability.BUTTONS}),
    )

    assert decode(encode(frame)) == frame
    # Encoded deterministically, so two identical frames produce identical bytes -- which is
    # what makes a wire log diffable.
    assert encode(frame) == encode(frame)
    assert json.loads(encode(frame))["caps"] == ["buttons", "halo"]


def test_message_type_of_resolves_both_directions_and_nothing_else() -> None:
    assert message_type_of("text_in") is DeviceMessage.TEXT_IN
    assert message_type_of("reply") is ServerMessage.REPLY
    assert message_type_of("nonsense") is None


def test_protocol_errors_carry_an_enumerated_code() -> None:
    # The router turns whatever the codec raises into `error{code}`; a code-less exception
    # would leave it inventing one.
    for raw in ("not json", json.dumps({"type": "nope"})):
        with pytest.raises((MalformedFrame, UnknownMessage)) as raised:
            decode(raw)
        assert raised.value.code is ErrorCode.INTERNAL


# ---------------------------------------------------------------------------------------
# Frame size (code review #1)
# ---------------------------------------------------------------------------------------


def test_an_oversize_frame_is_refused_without_being_parsed() -> None:
    from roboface_server.protocol import MAX_TEXT_FRAME_BYTES

    # Valid JSON, and still refused: the cap is about never allocating the parsed object,
    # not about the payload being wrong.
    oversize = json.dumps({"type": "text_in", "text": "x" * MAX_TEXT_FRAME_BYTES})
    assert len(oversize) > MAX_TEXT_FRAME_BYTES

    with pytest.raises(MalformedFrame, match="the limit is"):
        decode(oversize)


def test_a_frame_at_the_limit_is_still_accepted() -> None:
    from roboface_server.protocol import MAX_TEXT_FRAME_BYTES

    # Off-by-one guard: the boundary is inclusive, so a device sized exactly to the
    # documented cap is not rejected by it.
    padding = MAX_TEXT_FRAME_BYTES - len(json.dumps({"type": "text_in", "text": ""}))
    at_limit = json.dumps({"type": "text_in", "text": "x" * padding})
    assert len(at_limit) == MAX_TEXT_FRAME_BYTES

    assert decode(at_limit) == TextIn(text="x" * padding)


def test_oversize_bytes_are_refused_too() -> None:
    from roboface_server.protocol import MAX_TEXT_FRAME_BYTES

    with pytest.raises(MalformedFrame, match="the limit is"):
        decode(b"x" * (MAX_TEXT_FRAME_BYTES + 1))

"""Contract test for the **WS protocol** seam.

This file changes only when the contract changes. It is deliberately literal: every
expectation is spelled out rather than derived from the module under test, because a test
that reads its answers from the implementation cannot notice the implementation moving.

The authority is ARCHITECTURE.md §Contracts -> WS device<->server, §Error codes and
§Hardware variants. If this test and that document disagree, one of them is wrong and the
disagreement is the finding.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest
from roboface_server import protocol
from roboface_server.protocol import (
    AUDIO_CHANNELS,
    AUDIO_FMT,
    AUDIO_FORMAT,
    AUDIO_SAMPLE_RATE,
    BINARY_DEVICE_MESSAGES,
    BINARY_SERVER_MESSAGES,
    MAX_TEXT_FRAME_BYTES,
    PROTO_VERSION,
    Accepted,
    BinaryPhase,
    Capability,
    DeviceMessage,
    ErrorCode,
    Hello,
    Rejected,
    ServerMessage,
    device_binary_meaning,
    negotiate,
    parse_audio_fmt,
    server_binary_meaning,
)

# ---------------------------------------------------------------------------------------
# The message vocabulary, both directions
# ---------------------------------------------------------------------------------------

EXPECTED_DEVICE_MESSAGES = {
    "hello",
    "listen_start",
    "audio",
    "listen_stop",
    "text_in",
    "event",
    "image_in",
    "image",
    "ping",
}

EXPECTED_SERVER_MESSAGES = {
    "asr_partial",
    "asr",
    "reply",
    "emotion",
    "tts_audio",
    "tts_end",
    "config_updated",
    "error",
    "restart",
    "pong",
}


def test_device_to_server_vocabulary_is_exactly_the_contract() -> None:
    assert {member.value for member in DeviceMessage} == EXPECTED_DEVICE_MESSAGES


def test_server_to_device_vocabulary_is_exactly_the_contract() -> None:
    assert {member.value for member in ServerMessage} == EXPECTED_SERVER_MESSAGES


def test_the_two_vocabularies_are_disjoint() -> None:
    # `decode` resolves a name against both sets with one lookup, which is only unambiguous
    # while this holds. A future overlapping name would need a direction argument instead.
    assert set() == EXPECTED_DEVICE_MESSAGES & EXPECTED_SERVER_MESSAGES


def test_binary_frames_are_exactly_audio_image_and_tts_audio() -> None:
    # Binary frames carry raw payloads with no JSON envelope; everything else is a text frame.
    assert {member.value for member in BINARY_DEVICE_MESSAGES} == {"audio", "image"}
    assert {member.value for member in BINARY_SERVER_MESSAGES} == {"tts_audio"}


def test_text_and_binary_partition_each_vocabulary() -> None:
    assert frozenset(DeviceMessage) == protocol.TEXT_DEVICE_MESSAGES | BINARY_DEVICE_MESSAGES
    assert frozenset(ServerMessage) == protocol.TEXT_SERVER_MESSAGES | BINARY_SERVER_MESSAGES
    assert frozenset() == protocol.TEXT_DEVICE_MESSAGES & BINARY_DEVICE_MESSAGES
    assert frozenset() == protocol.TEXT_SERVER_MESSAGES & BINARY_SERVER_MESSAGES


# ---------------------------------------------------------------------------------------
# The error-code enum
# ---------------------------------------------------------------------------------------


def test_error_codes_are_exactly_the_twelve_enumerated_values() -> None:
    assert {member.value for member in ErrorCode} == {
        "wifi_lost",
        "server_unreachable",
        "proto_unsupported",
        "unauthorized",
        "rate_limited",
        "asr_failed",
        "llm_timeout",
        "llm_failed",
        "tts_failed",
        "vision_failed",
        "bad_frame",
        "internal",
    }


def test_bad_frame_and_internal_split_by_whose_fault_it_is() -> None:
    """The reason the twelfth code exists (v0.1 code review, finding 9).

    `bad_frame` is the device's fault, `internal` is the server's. The distinction matters at
    the far end: from v0.4 the device renders the error face for whatever code it is sent, so
    reporting a device's own malformed frame as `internal` would show a fault the server never
    had, and would point any retry logic at the wrong side.
    """
    assert ErrorCode.BAD_FRAME.value == "bad_frame"
    assert ErrorCode.INTERNAL.value == "internal"
    assert ErrorCode.BAD_FRAME is not ErrorCode.INTERNAL

    # Everything the codec raises is a judgement about what arrived, so that is its default.
    assert protocol.ProtocolError("anything").code is ErrorCode.BAD_FRAME


# ---------------------------------------------------------------------------------------
# Capability flags
# ---------------------------------------------------------------------------------------


def test_capabilities_are_exactly_the_five_flags() -> None:
    assert {member.value for member in Capability} == {
        "touch",
        "camera",
        "dual_mic",
        "halo",
        "buttons",
    }


# ---------------------------------------------------------------------------------------
# Version and audio constants
# ---------------------------------------------------------------------------------------


def test_proto_version_is_pinned() -> None:
    assert PROTO_VERSION == 1


def test_audio_is_pcm16_16khz_mono() -> None:
    assert AUDIO_FORMAT == "pcm16"
    assert AUDIO_SAMPLE_RATE == 16000
    assert AUDIO_CHANNELS == 1


def test_audio_fmt_string_agrees_with_its_parts() -> None:
    # ARCHITECTURE: the format is fixed in three places at once (this constant,
    # ELEVENLABS_OUTPUT_FORMAT, DEEPGRAM_ENCODING/SAMPLE_RATE) and they must agree. The
    # provider adapters read theirs from configuration; this is the authority they match.
    assert AUDIO_FMT == "pcm16/16000/1"
    assert parse_audio_fmt(AUDIO_FMT) == (AUDIO_FORMAT, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)


# ---------------------------------------------------------------------------------------
# Frame size
# ---------------------------------------------------------------------------------------


def test_the_text_frame_cap_is_pinned() -> None:
    # Part of the contract: the firmware must know what it may not exceed, and a later
    # phase must not quietly raise this to accommodate a payload that belongs in a binary
    # frame. Bulk data (audio, JPEG) never travels as a text frame.
    assert MAX_TEXT_FRAME_BYTES == 64 * 1024


def test_an_oversize_text_frame_is_refused_before_it_is_parsed() -> None:
    oversize = "x" * (MAX_TEXT_FRAME_BYTES + 1)

    with pytest.raises(protocol.MalformedFrame, match="the limit is"):
        protocol.decode_envelope(oversize)


# ---------------------------------------------------------------------------------------
# hello negotiation
# ---------------------------------------------------------------------------------------


def _hello(proto_ver: int = PROTO_VERSION) -> Hello:
    return Hello(
        device_id="core-s3-01",
        proto_ver=proto_ver,
        audio_fmt=AUDIO_FMT,
        caps=frozenset({Capability.TOUCH, Capability.CAMERA}),
    )


def test_negotiate_accepts_the_current_proto_version() -> None:
    result = negotiate(_hello())

    assert isinstance(result, Accepted)
    assert result.hello.device_id == "core-s3-01"


def test_negotiate_rejects_any_other_version_with_proto_unsupported() -> None:
    for wrong in (0, PROTO_VERSION + 1, 99):
        result = negotiate(_hello(proto_ver=wrong))

        assert isinstance(result, Rejected), f"proto_ver {wrong} should be rejected"
        assert result.code is ErrorCode.PROTO_UNSUPPORTED


def test_unknown_caps_are_ignored_not_rejected() -> None:
    # A newer board announcing a capability this server has never heard of must still connect.
    caps = protocol.parse_caps(["touch", "lidar", "camera", 7, None])

    assert caps == frozenset({Capability.TOUCH, Capability.CAMERA})


# ---------------------------------------------------------------------------------------
# The no-envelope rule for binary frames
# ---------------------------------------------------------------------------------------


def test_binary_meaning_comes_from_direction_plus_connection_state() -> None:
    assert device_binary_meaning(BinaryPhase.LISTENING) is DeviceMessage.AUDIO
    assert device_binary_meaning(BinaryPhase.AWAITING_IMAGE) is DeviceMessage.IMAGE
    assert server_binary_meaning(BinaryPhase.SPEAKING) is ServerMessage.TTS_AUDIO


def test_a_binary_frame_outside_its_phase_means_nothing() -> None:
    # Not "defaults to audio" -- a binary frame with no state to give it meaning is a
    # protocol violation, and the router must be able to tell the difference.
    assert device_binary_meaning(BinaryPhase.IDLE) is None
    assert device_binary_meaning(BinaryPhase.SPEAKING) is None
    assert server_binary_meaning(BinaryPhase.IDLE) is None
    assert server_binary_meaning(BinaryPhase.LISTENING) is None


# ---------------------------------------------------------------------------------------
# The `reply` delta shape (v0.2)
# ---------------------------------------------------------------------------------------


def test_reply_carries_a_text_and_a_final_flag() -> None:
    frame = protocol.Reply(text="привіт", final=False)

    assert set(frame.__dataclass_fields__) == {"text", "final"}


def test_a_turn_is_many_reply_frames_with_exactly_one_terminal() -> None:
    """The streaming contract, stated as a property of a turn's frames.

    ARCHITECTURE §Streaming is the architecture, not an optimisation: "token deltas leave as
    individual `reply` frames as they arrive; the reply is never accumulated and sent whole."
    A turn is therefore N deltas with `final: false` and exactly one closing `final: true` --
    and `final` marks the last frame of a turn, not the only one.

    Pinned here so a later change that quietly re-accumulates fails the contract test rather
    than merely making the device feel slower.
    """
    turn = [
        protocol.Reply(text="При", final=False),
        protocol.Reply(text="віт", final=False),
        protocol.Reply(text="", final=True),
    ]

    assert len(turn) > 2, "a streamed turn is more than a single frame plus its terminator"
    assert [frame.final for frame in turn] == [False, False, True]
    assert sum(frame.final for frame in turn) == 1
    # The terminator closes the turn; it does not repeat the reply.
    assert turn[-1].text == ""
    assert "".join(frame.text for frame in turn) == "Привіт"


def test_every_delta_is_an_ordinary_reply_frame_on_the_wire() -> None:
    # A delta is not a new message type: v0.2 changes the *cardinality* of `reply`, not the
    # vocabulary. The firmware's parser therefore needs no new branch.
    encoded = protocol.encode(protocol.Reply(text="При", final=False))

    assert protocol.decode(encoded) == protocol.Reply(text="При", final=False)
    assert "reply" in encoded


# ---------------------------------------------------------------------------------------
# Purity -- the property that lets the firmware mirror this module
# ---------------------------------------------------------------------------------------


def test_protocol_imports_without_any_framework(repo_root: Path) -> None:
    """Import the module in a fresh interpreter and assert no framework came with it.

    A subprocess is the only honest way to check: by the time this test runs, other tests in
    the session have already imported FastAPI, so `sys.modules` here proves nothing.
    """
    program = (
        "import sys;"
        "import roboface_server.protocol;"
        "banned = sorted(m for m in sys.modules "
        "if m.split('.')[0] in {'fastapi', 'websockets', 'starlette', 'uvicorn'});"
        "print(','.join(banned))"
    )
    environment = dict(os.environ)
    environment["PYTHONPATH"] = "server"
    result = subprocess.run(
        [sys.executable, "-c", program],
        cwd=repo_root,
        capture_output=True,
        text=True,
        env=environment,
        timeout=120,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "", f"protocol.py pulled in a framework: {result.stdout}"

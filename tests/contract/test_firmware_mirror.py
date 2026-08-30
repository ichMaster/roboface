"""The firmware's copy of the wire contract, checked against the server's.

`firmware/src/pure/ws_protocol.h` is the device half of the WS protocol seam. Two copies of a
contract in two languages drift silently — one side gains a value, the other does not, and nothing
fails until a board is in front of you.

So this file reads the C++ header and compares it to `protocol.py`. It runs in the **Python** job,
which runs on every push, so a drift is caught even by someone who never invokes `pio`. The C++
contract test (`firmware/test/test_ws_protocol/`) pins the same facts from the other side; these two
files are one contract checked twice, and a disagreement between them means one is wrong.

The `hello` / `text_in` / `ping` literals below are byte-for-byte what the firmware actually builds
— the same strings its own test asserts — and they are fed to the server's real `decode()` here.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from roboface_server.protocol import (
    AUDIO_FMT,
    DEFAULT_INTENSITY,
    DEFAULT_TTL_MS,
    MAX_TEXT_FRAME_BYTES,
    PROTO_VERSION,
    Capability,
    DeviceMessage,
    Emotion,
    ErrorCode,
    Hello,
    Ping,
    ServerMessage,
    TextIn,
    decode,
)

HEADER = Path(__file__).resolve().parents[2] / "firmware" / "src" / "pure" / "ws_protocol.h"

#: Exactly what `roboface::buildHello("core-s3-01")` emits, asserted in the firmware's own test.
FIRMWARE_HELLO = (
    '{"type":"hello","device_id":"core-s3-01","proto_ver":1,'
    '"audio_fmt":"pcm16/16000/1","caps":["camera","dual_mic","touch"]}'
)
FIRMWARE_TEXT_IN = '{"type":"text_in","text":"привіт"}'
FIRMWARE_PING = '{"type":"ping"}'


@pytest.fixture(scope="module")
def header_source() -> str:
    if not HEADER.is_file():
        pytest.skip("firmware/ is not present in this checkout")
    return HEADER.read_text(encoding="utf-8")


def _string_constant(source: str, name: str) -> str:
    match = re.search(rf'{name}\s*=\s*"([^"]*)"', source)
    assert match, f"{name} not found in ws_protocol.h"
    return match.group(1)


def _int_constant(source: str, name: str) -> int:
    match = re.search(rf"{name}\s*=\s*(\d+)", source)
    assert match, f"{name} not found in ws_protocol.h"
    return int(match.group(1))


def _quoted_returns(source: str, enum_name: str) -> set[str]:
    """The string literals the `toString(<enum>)` switch returns.

    Bounded by the function's own closing brace at column 0 rather than by a sentinel `return`
    — `toString(ErrorCode)` ends with `return "unknown";`, and assuming one spelling for all of
    them made this helper agree with two of the three enums and silently run past the third.
    """
    start = source.index(f"toString({enum_name} ")
    end = source.index("\n}\n", start)
    return set(re.findall(r'return "([^"]+)";', source[start:end]))


def _includes(source: str) -> set[str]:
    """Only real `#include` directives.

    Not the header's own prose about what it must not include, which is what a plain substring
    search matched the first time this test was written.
    """
    return set(re.findall(r"^\s*#include\s+(<[^>]+>|\"[^\"]+\")", source, re.MULTILINE))


# ---------------------------------------------------------------------------------------
# The firmware's frames are what the server accepts
# ---------------------------------------------------------------------------------------


def test_the_firmware_hello_is_accepted_by_the_server() -> None:
    frame = decode(FIRMWARE_HELLO)

    assert frame == Hello(
        device_id="core-s3-01",
        proto_ver=PROTO_VERSION,
        audio_fmt=AUDIO_FMT,
        caps=frozenset({Capability.CAMERA, Capability.DUAL_MIC, Capability.TOUCH}),
    )


def test_the_firmware_announces_the_core_s3_hardware_and_no_more() -> None:
    # Not halo (the optional Bottom3, v5) and not buttons (the FIRE, v6). The server tailors what
    # it sends to `caps`, so overstating them means asking for frames this board cannot act on.
    frame = decode(FIRMWARE_HELLO)
    assert isinstance(frame, Hello)

    assert Capability.HALO not in frame.caps
    assert Capability.BUTTONS not in frame.caps


def test_the_firmware_text_in_and_ping_are_accepted() -> None:
    assert decode(FIRMWARE_TEXT_IN) == TextIn(text="привіт")
    assert decode(FIRMWARE_PING) == Ping()


# ---------------------------------------------------------------------------------------
# The constants agree across the two languages
# ---------------------------------------------------------------------------------------


def test_proto_version_agrees(header_source: str) -> None:
    assert _int_constant(header_source, "kProtoVersion") == PROTO_VERSION


def test_audio_format_agrees(header_source: str) -> None:
    assert _string_constant(header_source, "kAudioFmt") == AUDIO_FMT


def test_the_text_frame_cap_agrees(header_source: str) -> None:
    # The device applies it inbound too, where it matters more: 320 KB of RAM, not gigabytes.
    assert _int_constant(header_source, "kMaxTextFrameBytes") == MAX_TEXT_FRAME_BYTES


# ---------------------------------------------------------------------------------------
# The vocabularies agree
# ---------------------------------------------------------------------------------------


def test_the_device_vocabulary_agrees(header_source: str) -> None:
    assert _quoted_returns(header_source, "DeviceMessage") == {m.value for m in DeviceMessage}


def test_the_server_vocabulary_agrees(header_source: str) -> None:
    assert _quoted_returns(header_source, "ServerMessage") == {m.value for m in ServerMessage}


def test_the_error_codes_agree(header_source: str) -> None:
    """All twelve, including bad_frame.

    The firmware adds one value the contract does not have — "unknown" — so a code added
    server-side after a board was flashed still arrives as a fault rather than as a parse
    failure. That is the only permitted difference, and it is asserted rather than tolerated.
    """
    firmware_codes = _quoted_returns(header_source, "ErrorCode")
    server_codes = {code.value for code in ErrorCode}

    assert len(server_codes) == 12
    assert firmware_codes - server_codes == {"unknown"}
    assert server_codes - firmware_codes == set()


def test_the_firmware_carries_bad_frame(header_source: str) -> None:
    # The v0.1 review's finding 9, decided and implemented in v0.2. A firmware that predated it
    # would render `internal` for its own malformed frame and blame the server.
    assert "bad_frame" in _quoted_returns(header_source, "ErrorCode")


# ---------------------------------------------------------------------------------------
# Purity
# ---------------------------------------------------------------------------------------


def test_the_header_stays_free_of_the_arduino_world(header_source: str) -> None:
    """Belt and braces with the `native` build filter.

    The filter catches it at compile time; this catches it in a job that always runs, and says
    why in the failure message rather than as a linker error.
    """
    includes = _includes(header_source)

    for banned in ("<M5Unified.h>", "<WiFi.h>", "<WebSocketsClient.h>", "<Arduino.h>", "<M5GFX.h>"):
        assert banned not in includes, f"pure code must not include {banned}"

    # And what it *does* include is ArduinoJson plus the C++ standard library — ArduinoJson
    # despite the name is host-compilable, which is why parsing can live in the tested half.
    assert "<ArduinoJson.h>" in includes


def test_the_emotion_enum_is_identical_on_both_sides() -> None:
    """`EmotionFrame`'s vocabulary, checked across the two languages.

    This is the drift that would be silent and expensive. The server's `Emotion` and the firmware's
    are two hand-written lists of the same seven words; if one gains a value the other has never
    heard of, nothing fails — `emotionFrom` coerces it to `neutral` and the device shows a resting
    face for an emotion the server believes it sent. No error, no log line, and the only symptom is
    a character that seems oddly flat.

    So: same words, same order. The order matters because both sides index the enum by position in
    their own tests, and because `neutral` being first is what everything relaxes to.
    """
    header = (Path(__file__).resolve().parents[2] / "firmware/src/pure/face.h").read_text()
    block = re.search(r"enum class Emotion : uint8_t \{(.*?)\};", header, re.S)
    assert block is not None, "the firmware's Emotion enum moved or was renamed"

    firmware = [
        name[0].lower() + name[1:]
        for name in re.findall(r"\bk([A-Z]\w*)", block.group(1))
        if name != "Count"
    ]
    assert firmware == [member.value for member in Emotion]


def test_the_frame_defaults_are_identical_on_both_sides() -> None:
    """Both halves apply these, which is what lets the server omit every optional field still at
    its default. A disagreement is a face that differs from the one that was sent, and nothing
    anywhere would report it."""
    header = (Path(__file__).resolve().parents[2] / "firmware/src/pure/face.h").read_text()

    ttl = re.search(r"kDefaultTtlMs = (\d+)", header)
    intensity = re.search(r"kDefaultIntensity = ([\d.]+)f", header)
    assert ttl is not None and intensity is not None

    assert int(ttl.group(1)) == DEFAULT_TTL_MS
    assert float(intensity.group(1)) == DEFAULT_INTENSITY

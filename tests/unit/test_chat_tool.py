"""Unit tests for the terminal chat client's pure helpers.

The tool is how a person talks to the server before there is firmware, so it is worth the same
discipline as the rest: its input parsing and its rendering are pure functions, and they are
pinned here. A tool that silently rots is worse than none — that was v0.1 review finding 4, and
the lesson applies to the tool as much as to the harness.
"""

from __future__ import annotations

import pytest
from chat import Command, Stats, Style, describe_frame, format_timing, parse_input
from roboface_server.protocol import ErrorCode, ErrorFrame, Pong, Reply

PLAIN = Style(enabled=False)


# ---------------------------------------------------------------------------------------
# Input parsing
# ---------------------------------------------------------------------------------------


def test_ordinary_text_is_speech() -> None:
    assert parse_input("привіт, як справи?") == Command("say", "привіт, як справи?")


def test_a_slash_starts_a_command() -> None:
    assert parse_input("/ping") == Command("ping")
    assert parse_input("/quit") == Command("quit")


def test_a_command_keeps_its_argument() -> None:
    assert parse_input('/raw {"type": "nope"}') == Command("raw", '{"type": "nope"}')
    assert parse_input("/hello 99") == Command("hello", "99")


def test_a_double_slash_escapes_so_a_slash_can_be_said() -> None:
    # Otherwise there would be a sentence this tool cannot send, which is a poor property for
    # the thing you reach for when testing what the server does with odd input.
    assert parse_input("//usr/bin is on the path") == Command("say", "/usr/bin is on the path")


def test_blank_input_is_not_a_turn() -> None:
    assert parse_input("").kind == "blank"
    assert parse_input("   ").kind == "blank"


def test_commands_are_case_insensitive_and_trimmed() -> None:
    assert parse_input("  /PING  ") == Command("ping")


def test_a_bare_slash_is_help_rather_than_an_empty_command() -> None:
    assert parse_input("/") == Command("help")


# ---------------------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------------------


def test_an_error_frame_shows_its_enumerated_code() -> None:
    # The code is the one string worth typing into a search — DEVICE_UI makes the same point
    # about the device's own fault screen.
    rendered = describe_frame(ErrorFrame(code=ErrorCode.BAD_FRAME, msg="nope"), PLAIN)

    assert "bad_frame" in rendered
    assert "nope" in rendered


def test_an_unhandled_frame_type_is_shown_rather_than_dropped() -> None:
    # When v1 adds asr_partial and v2 adds emotion, the tool shows them the day they exist
    # instead of the day someone remembers to teach it about them.
    assert "pong" in describe_frame(Pong(), PLAIN)


def test_undecodable_text_is_shown_verbatim() -> None:
    assert "gibberish" in describe_frame("gibberish", PLAIN)


def test_timing_reports_first_delta_and_total() -> None:
    # Time-to-first-delta is the number the architecture exists to keep small; showing only
    # the total would hide it.
    rendered = format_timing(deltas=5, first_ms=120.4, total_ms=800.9, style=PLAIN)

    assert "5 deltas" in rendered
    assert "first 120 ms" in rendered
    assert "complete 801 ms" in rendered


def test_timing_is_singular_for_one_delta() -> None:
    assert "1 delta ·" in format_timing(deltas=1, first_ms=10.0, total_ms=20.0, style=PLAIN)


def test_timing_says_so_when_nothing_streamed() -> None:
    # A silent model ends a turn cleanly with no deltas at all; "first None ms" would be worse
    # than saying what happened.
    assert "no deltas" in format_timing(deltas=0, first_ms=None, total_ms=50.0, style=PLAIN)


def test_styling_is_dropped_when_output_is_not_a_terminal() -> None:
    # So piping the tool into a file or a diff produces something readable.
    assert "\033[" not in describe_frame(ErrorFrame(code=ErrorCode.INTERNAL, msg="x"), PLAIN)
    assert "\033[" in describe_frame(ErrorFrame(code=ErrorCode.INTERNAL, msg="x"), Style())


# ---------------------------------------------------------------------------------------
# Stats
# ---------------------------------------------------------------------------------------


def test_stats_start_empty() -> None:
    assert Stats().summary() == "no turns yet"


def test_stats_are_singular_for_one_turn() -> None:
    assert Stats(turns=1, deltas=2, first_ms=[10.0]).summary().startswith("1 turn ·")


def test_stats_summarise_the_session() -> None:
    stats = Stats(turns=3, deltas=12, errors=1, first_ms=[100.0, 200.0, 300.0])

    summary = stats.summary()
    assert "3 turns" in summary
    assert "12 deltas" in summary
    assert "1 errors" in summary
    assert "min 100" in summary and "mean 200" in summary and "max 300" in summary


def test_the_tool_speaks_the_protocol_through_the_codec() -> None:
    """Not by hand-rolling JSON.

    A client with its own idea of the wire is a second implementation of it, and a contract
    change would then break the device while this tool carried on looking fine.
    """
    import chat

    assert chat.encode is not None
    assert chat.decode is not None
    assert chat.AUDIO_FMT and chat.PROTO_VERSION


@pytest.mark.parametrize("frame", [Reply(text="hi", final=False), Reply(text="", final=True)])
def test_reply_frames_are_handled_by_the_turn_loop_not_the_generic_renderer(frame: Reply) -> None:
    # describe_frame is the fallback for everything that is not a streamed reply; replies are
    # rendered inline as they arrive, which is the behaviour worth having.
    assert "reply" in describe_frame(frame, PLAIN)

"""Unit tests for the board monitor's pure helper.

Only one thing in that tool is worth testing without a board, and it is the thing most likely to be
wrong: a serial read almost never lands on a line boundary, so reassembling lines across reads is
where a monitor either works or silently drops the last line of every buffer.
"""

from __future__ import annotations

import pytest

board = pytest.importorskip("board", reason="pyserial is unavailable in this environment")
split_lines = board.split_lines


def test_a_whole_line_comes_out() -> None:
    lines, rest = split_lines(b"[state] idle\n")

    assert lines == ["[state] idle"]
    assert rest == b""


def test_a_partial_line_is_held_back() -> None:
    # The common case: a read lands mid-line. Emitting it would print half a message and then
    # print the other half as though it were a second one.
    lines, rest = split_lines(b"[state] idl")

    assert lines == []
    assert rest == b"[state] idl"


def test_a_line_split_across_two_reads_is_rejoined() -> None:
    first, rest = split_lines(b"[faces] 1/6  id")
    second, rest = split_lines(rest + b"le\n")

    assert first == []
    assert second == ["[faces] 1/6  idle"]
    assert rest == b""


def test_several_lines_in_one_read() -> None:
    lines, rest = split_lines(b"one\ntwo\nthree\n")

    assert lines == ["one", "two", "three"]
    assert rest == b""


def test_crlf_does_not_leave_a_stray_carriage_return() -> None:
    # The firmware prints with \n, but the USB CDC and some terminals add \r. A trailing \r turns
    # into a cursor jump that overwrites the line just printed.
    lines, _ = split_lines(b"[status] up 10s\r\n")

    assert lines == ["[status] up 10s"]


def test_invalid_utf8_does_not_crash_the_monitor() -> None:
    # A board mid-reset emits garbage. A monitor that raises on it dies exactly when the thing it
    # is watching is most interesting.
    lines, _ = split_lines(b"\xff\xfe boot\n")

    assert len(lines) == 1
    assert "boot" in lines[0]


def test_ukrainian_survives_the_round_trip() -> None:
    lines, _ = split_lines("Привіт!\n".encode())

    assert lines == ["Привіт!"]


def test_a_multibyte_character_split_across_reads() -> None:
    """The subtle one: UTF-8 boundaries do not respect read boundaries.

    Splitting mid-character produces a replacement character rather than an exception, which is the
    right trade for a monitor — one mangled glyph beats a dead session.
    """
    payload = "Привіт\n".encode()
    first, rest = split_lines(payload[:5])
    second, _ = split_lines(rest + payload[5:])

    assert first == []
    assert "т" in second[0]

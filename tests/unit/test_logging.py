"""Unit tests for structured logging.

The three properties asserted here are the ones that make a log worth having: every line
identifies its connection, the level actually gates output, and no body or secret ever
reaches the stream.
"""

from __future__ import annotations

import json
from io import StringIO

import pytest
from roboface_server.logging import (
    LOGGER_NAME,
    bind_device,
    chars,
    configure,
    connection_context,
    current_context,
    log,
)


def _lines(stream: StringIO) -> list[dict[str, object]]:
    return [json.loads(line) for line in stream.getvalue().splitlines() if line]


@pytest.fixture
def stream() -> StringIO:
    captured = StringIO()
    configure("debug", stream=captured)
    return captured


# ---------------------------------------------------------------------------------------
# Identity on every line
# ---------------------------------------------------------------------------------------


def test_a_bound_context_appears_on_every_line(stream: StringIO) -> None:
    with connection_context("session-1"):
        bind_device("core-s3-01")
        log("first")
        log("second", extra_field=7)

    lines = _lines(stream)
    assert len(lines) == 2
    for line in lines:
        assert line["session_id"] == "session-1"
        assert line["device_id"] == "core-s3-01"


def test_lines_before_hello_carry_an_explicit_null_device_id(stream: StringIO) -> None:
    # The shape of a line must not depend on how far the connection got.
    with connection_context("session-1"):
        log("connection.accepted")

    (line,) = _lines(stream)
    assert "device_id" in line
    assert line["device_id"] is None
    assert line["session_id"] == "session-1"


def test_every_line_has_the_same_keys(stream: StringIO) -> None:
    with connection_context("session-1"):
        log("before")
        bind_device("d")
        log("after")

    before, after = _lines(stream)
    base = {"ts", "level", "event", "device_id", "session_id"}
    assert base <= set(before) and base <= set(after)


def test_the_context_is_released_on_exit(stream: StringIO) -> None:
    # A pooled worker must not carry one device's identity into the next connection.
    with connection_context("session-1"):
        bind_device("core-s3-01")
    log("outside")

    (line,) = _lines(stream)
    assert line["session_id"] is None
    assert line["device_id"] is None
    assert current_context().device_id is None


def test_fields_are_merged_into_the_line(stream: StringIO) -> None:
    with connection_context("s"):
        log("hello.negotiated", proto_ver=1, caps=["touch", "camera"])

    (line,) = _lines(stream)
    assert line["event"] == "hello.negotiated"
    assert line["proto_ver"] == 1
    assert line["caps"] == ["touch", "camera"]


# ---------------------------------------------------------------------------------------
# Level gating
# ---------------------------------------------------------------------------------------


def test_debug_is_suppressed_at_info() -> None:
    captured = StringIO()
    configure("info", stream=captured)

    with connection_context("s"):
        log("chatter", level="debug")
        log("worth_knowing", level="info")

    assert [line["event"] for line in _lines(captured)] == ["worth_knowing"]


def test_debug_is_emitted_at_debug() -> None:
    captured = StringIO()
    configure("debug", stream=captured)

    with connection_context("s"):
        log("chatter", level="debug")

    assert [line["event"] for line in _lines(captured)] == ["chatter"]


@pytest.mark.parametrize("level", ["debug", "info", "warning", "error", "critical"])
def test_every_configured_level_is_accepted(level: str) -> None:
    captured = StringIO()
    configure(level, stream=captured)

    with connection_context("s"):
        log("at_the_threshold", level=level)

    assert [line["level"] for line in _lines(captured)] == [level]


def test_configure_is_idempotent() -> None:
    # Called twice, a line must appear once -- a duplicated handler doubles the whole log.
    captured = StringIO()
    configure("info", stream=captured)
    configure("info", stream=captured)

    with connection_context("s"):
        log("once")

    assert len(_lines(captured)) == 1


def test_the_logger_does_not_propagate(stream: StringIO) -> None:
    import logging as stdlib_logging

    # Propagation would hand every line to the root handler as well -- unformatted, and
    # duplicated wherever a deployment configures one.
    assert stdlib_logging.getLogger(LOGGER_NAME).propagate is False


# ---------------------------------------------------------------------------------------
# Redaction
# ---------------------------------------------------------------------------------------


def test_chars_measures_rather_than_reveals() -> None:
    assert chars("привіт") == 6
    assert chars("") == 0


def test_a_body_logged_as_a_length_does_not_appear(stream: StringIO) -> None:
    secret_ish = "my bank pin is 4171 and I would rather not see it in a log"

    with connection_context("s"):
        log("turn.text_in", chars=chars(secret_ish))

    raw = stream.getvalue()
    assert secret_ish not in raw
    assert "4171" not in raw
    assert json.loads(raw)["chars"] == len(secret_ish)

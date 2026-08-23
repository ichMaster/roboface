"""The log, observed over a real connection.

The unit tests pin the mechanism; these pin that the router actually uses it -- that a real
turn produces the lifecycle a reader needs, and that two simultaneous devices do not bleed
into each other's lines. The second is the property `contextvars` is there to guarantee, and
the only way to check it is to have two connections open at once.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from fake_device import FakeDevice, connect
from fastapi import FastAPI
from roboface_server.protocol import PROTO_VERSION, Capability, ErrorFrame, Ping, TextIn

LogReader = Callable[[], list[dict[str, Any]]]

# NOTE `log_lines` is requested *before* `device` throughout this file. Same-scope fixtures
# are built in parameter order, and the `device` fixture opens the socket as it is built --
# so requesting it first would lose `connection.accepted`, the one line emitted before the
# test body runs.


def test_a_full_turn_logs_its_lifecycle(log_lines: LogReader, device: FakeDevice) -> None:
    device.hello(caps=frozenset({Capability.TOUCH}))
    device.send(TextIn(text="привіт"))
    device.collect_reply()

    events = [line["event"] for line in log_lines()]

    assert "connection.accepted" in events
    assert "hello.negotiated" in events
    assert "turn.text_in" in events
    # v0.2: the router logs how many deltas it forwarded; the orchestrator logs the reply
    # itself. With the echo responder there is no orchestrator, so `turn.streamed` is the
    # one that must appear.
    assert "turn.streamed" in events


def test_hello_is_logged_with_its_version_and_caps(
    log_lines: LogReader, device: FakeDevice
) -> None:
    device.hello(caps=frozenset({Capability.TOUCH, Capability.CAMERA}))
    device.send(Ping())
    device.recv()

    negotiated = next(line for line in log_lines() if line["event"] == "hello.negotiated")

    assert negotiated["proto_ver"] == PROTO_VERSION
    assert sorted(negotiated["caps"]) == ["camera", "touch"]
    assert negotiated["device_id"] == "fake-core-s3"


def test_every_line_of_a_connection_carries_its_session_id(
    log_lines: LogReader, device: FakeDevice
) -> None:
    device.hello()
    device.send(TextIn(text="anything"))
    device.collect_reply()

    lines = log_lines()
    assert lines, "the connection produced no log at all"
    assert all(line["session_id"] for line in lines)


def test_pre_hello_lines_have_a_null_device_id(log_lines: LogReader, device: FakeDevice) -> None:
    device.hello()
    device.send(TextIn(text="x"))
    device.collect_reply()

    accepted = next(line for line in log_lines() if line["event"] == "connection.accepted")

    assert accepted["device_id"] is None
    assert accepted["session_id"] is not None


def test_an_error_is_logged_with_its_enumerated_code(
    log_lines: LogReader, device: FakeDevice
) -> None:
    device.hello(proto_ver=PROTO_VERSION + 1)
    device.recv_until(ErrorFrame)

    rejected = next(line for line in log_lines() if line["event"] == "hello.rejected")

    assert rejected["code"] == "proto_unsupported"


def test_teardown_is_logged_with_a_reason(app: FastAPI, log_lines: LogReader) -> None:
    with connect(app) as fake:
        fake.hello()
        fake.send(TextIn(text="bye"))
        fake.collect_reply()

    closed = next(line for line in log_lines() if line["event"] == "connection.closed")

    assert closed["reason"]


def test_the_text_body_never_reaches_the_log(log_lines: LogReader, device: FakeDevice) -> None:
    body = "my card number is 4111 1111 1111 1111"

    device.hello()
    device.send(TextIn(text=body))
    device.collect_reply()

    lines = log_lines()
    rendered = str(lines)
    assert body not in rendered
    assert "4111" not in rendered

    text_in = next(line for line in lines if line["event"] == "turn.text_in")
    assert text_in["chars"] == len(body)


def test_two_simultaneous_devices_do_not_contaminate_each_other(
    app: FastAPI, log_lines: LogReader
) -> None:
    with connect(app, device_id="device-a") as first, connect(app, device_id="device-b") as second:
        first.hello()
        first.send(TextIn(text="from a"))
        first.collect_reply()

        second.hello()
        second.send(TextIn(text="from b"))
        second.collect_reply()

    lines = log_lines()
    sessions = {line["session_id"] for line in lines}
    assert len(sessions) == 2, f"two connections shared a session id: {sessions}"

    # Every line that knows a device_id agrees with the session it belongs to.
    by_session: dict[str, set[str]] = {}
    for line in lines:
        if line["device_id"] is not None:
            by_session.setdefault(str(line["session_id"]), set()).add(str(line["device_id"]))

    assert all(len(devices) == 1 for devices in by_session.values()), by_session
    assert {next(iter(devices)) for devices in by_session.values()} == {"device-a", "device-b"}

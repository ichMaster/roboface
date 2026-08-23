"""The v0.1 Definition of Done, asserted from outside the server.

Roadmap §v0.1: "the fake device connects, negotiates `hello`, sends `text_in` and receives
a `reply`; an unsupported `proto_ver` is rejected with `proto_unsupported`". Everything here
runs through the real ASGI application -- adapter, router, codec -- with no hardware, no
network and no paid API. The responder is the echo; there is no model call in this phase.
"""

from __future__ import annotations

import pytest
from fake_device import FakeDevice, connect
from fastapi import FastAPI
from roboface_server.app import create_app
from roboface_server.protocol import (
    PROTO_VERSION,
    Capability,
    ErrorCode,
    ErrorFrame,
    Ping,
    Pong,
    TextIn,
)
from roboface_server.router import ConnectionRegistry


def test_a_greeted_device_gets_a_reply(device: FakeDevice) -> None:
    """The DoD's first clause: connect -> hello -> text_in -> reply.

    From v0.2 the reply is a **stream**: several `reply{final: false}` deltas closed by one
    `final: true`. Asserting the reassembled text alone would pass against a buffered
    implementation, so the delta count is asserted too.
    """
    device.hello()
    device.send(TextIn(text="привіт як справи"))

    reply = device.collect_reply()

    assert reply.text == "привіт як справи"
    assert len(reply) > 1, "the reply arrived as one frame -- it was buffered, not streamed"


def test_hello_carries_capabilities_through(app: FastAPI, registry: ConnectionRegistry) -> None:
    with connect(app) as fake:
        fake.hello(caps=frozenset({Capability.TOUCH, Capability.CAMERA}))
        fake.send(TextIn(text="ping the state machine"))
        fake.collect_reply()

        connection = registry.active()[0]
        assert connection.device_id == "fake-core-s3"
        assert connection.caps == frozenset({Capability.TOUCH, Capability.CAMERA})


def test_an_unsupported_proto_version_is_rejected_and_the_socket_closes(
    device: FakeDevice,
) -> None:
    """The DoD's second clause."""
    device.hello(proto_ver=PROTO_VERSION + 1)

    error = device.recv_until(ErrorFrame)

    assert error.code is ErrorCode.PROTO_UNSUPPORTED
    device.expect_closed()


def test_ping_is_answered_with_pong(device: FakeDevice) -> None:
    device.hello()
    device.send(Ping())

    assert device.recv_until(Pong) == Pong()


def test_a_malformed_frame_is_answered_and_the_connection_survives(device: FakeDevice) -> None:
    device.hello()
    device.send_raw("{not json at all")

    error = device.recv_until(ErrorFrame)
    assert error.code in set(ErrorCode)

    # The connection is still usable -- a bad frame is not a fatal event.
    device.send(TextIn(text="still here"))
    assert device.collect_reply().text == "still here"


def test_a_binary_frame_is_refused_in_v0_1(device: FakeDevice) -> None:
    # No listening window and no announced image exist yet, so a raw payload has no meaning.
    # The harness can already send one, which is what v1 will need.
    device.hello()
    device.send_binary(b"\x00\x01\x02\x03")

    assert device.recv_until(ErrorFrame).code in set(ErrorCode)


def test_the_registry_is_empty_after_the_device_disconnects(
    app: FastAPI, registry: ConnectionRegistry
) -> None:
    with connect(app) as fake:
        fake.hello()
        fake.send(TextIn(text="hello"))
        fake.collect_reply()
        assert len(registry) == 1

    assert len(registry) == 0, "the connection outlived its socket"


def test_two_devices_are_served_independently(registry: ConnectionRegistry) -> None:
    application = create_app(registry=registry)

    with connect(application, device_id="device-a") as first:
        first.hello()
        first.send(TextIn(text="from a"))
        assert first.collect_reply().text == "from a"

        with connect(application, device_id="device-b") as second:
            second.hello()
            second.send(TextIn(text="from b"))
            assert second.collect_reply().text == "from b"

            assert {conn.device_id for conn in registry.active()} == {"device-a", "device-b"}

    assert len(registry) == 0


@pytest.mark.parametrize("text", ["", "  ", "привіт, як справи?", "x" * 4096])
def test_the_turn_survives_awkward_text(device: FakeDevice, text: str) -> None:
    device.hello()
    device.send(TextIn(text=text))

    # Reassembled from however many deltas it took: joining them must reproduce the input
    # exactly, which is what proves the router forwarded each one unmodified and in order.
    assert device.collect_reply().text == text


def test_no_paid_provider_is_reachable_from_the_suite(app: FastAPI) -> None:
    """The responder is the echo; there is no provider package in this phase at all.

    Stated as a test rather than a comment because "no paid API in tests" is a rule the
    suite is supposed to enforce, not merely to have been written under.
    """
    router = app.state.router

    assert type(router.responder).__name__ == "EchoResponder"

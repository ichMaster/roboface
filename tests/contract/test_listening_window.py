"""The listening window: `listen_start`, binary `audio`, `listen_stop`.

The framing this pins is the same trick the outbound side uses, in reverse: a binary frame carries
no envelope, and what gives it meaning is that the connection is **listening**. That is only safe if
a binary frame outside the window is refused, so most of these tests are about the refusals.
"""

from __future__ import annotations

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.protocol import (
    MAX_UTTERANCE_BYTES,
    ErrorCode,
    ErrorFrame,
    ListenStart,
    ListenStop,
    Ping,
    Pong,
)
from roboface_server.router import ConnectionRegistry


def test_audio_outside_a_window_is_refused() -> None:
    with connect(create_app()) as device:
        device.hello()
        device.send_binary(b"\x00\x01" * 8)
        frame = device.recv()
        assert isinstance(frame, ErrorFrame)
        assert frame.code is ErrorCode.BAD_FRAME


def test_audio_inside_a_window_is_accepted_silently() -> None:
    # Silence is the correct answer: every accepted chunk answered with a frame would double the
    # traffic of an utterance for no information.
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(b"\x00\x01" * 160)
        device.send(ListenStop())
        device.send(Ping())
        assert isinstance(device.recv(), Pong), "no error should have been sent"


def test_a_second_listen_start_is_refused() -> None:
    # Not a no-op: it means the device and the server disagree about state, and a silently
    # restarted window loses whatever preceded it.
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStart())
        device.send(ListenStart())
        frame = device.recv()
        assert isinstance(frame, ErrorFrame)
        assert frame.code is ErrorCode.BAD_FRAME


def test_a_listen_stop_without_a_start_is_refused() -> None:
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStop())
        frame = device.recv()
        assert isinstance(frame, ErrorFrame)
        assert frame.code is ErrorCode.BAD_FRAME


def test_audio_after_the_window_closes_is_refused_again() -> None:
    # The window must actually close. A phase that opened and never reset would accept audio for
    # the rest of the connection.
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStart())
        device.send_binary(b"\x00\x01" * 8)
        device.send(ListenStop())
        device.send_binary(b"\x00\x01" * 8)
        frame = device.recv()
        assert isinstance(frame, ErrorFrame)
        assert frame.code is ErrorCode.BAD_FRAME


def test_an_oversize_utterance_is_ended_with_an_error() -> None:
    # Told, not silently half-heard. The cap is checked *before* storing, so the memory the cap
    # exists to protect is never taken.
    chunk = b"\x00\x01" * 8192  # 16 KiB
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStart())
        sent = 0
        frame = None
        while sent <= MAX_UTTERANCE_BYTES + len(chunk):
            device.send_binary(chunk)
            sent += len(chunk)
            if sent > MAX_UTTERANCE_BYTES:
                frame = device.recv()
                break
        assert isinstance(frame, ErrorFrame)
        assert frame.code is ErrorCode.BAD_FRAME
        assert "exceeded" in frame.msg


def test_the_window_reopens_after_an_oversize_utterance() -> None:
    # The connection survives. An utterance that was too long is the person's problem, not the
    # session's -- v0.2's review made a failed turn keep the session, and this is the same rule.
    chunk = b"\x00\x01" * 8192
    with connect(create_app()) as device:
        device.hello()
        device.send(ListenStart())
        sent = 0
        while sent <= MAX_UTTERANCE_BYTES + len(chunk):
            device.send_binary(chunk)
            sent += len(chunk)
            if sent > MAX_UTTERANCE_BYTES:
                device.recv()
                break
        device.send(ListenStart())
        device.send_binary(b"\x00\x01" * 8)
        device.send(ListenStop())
        device.send(Ping())
        assert isinstance(device.recv(), Pong)


def test_a_disconnect_mid_utterance_releases_the_buffer() -> None:
    # The cap bounds a live utterance; it does not bound how many abandoned ones may accumulate.
    # A device on a flaky link reconnecting mid-sentence would leave one behind per attempt.
    registry = ConnectionRegistry()
    app = create_app(registry=registry)
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        device.send_binary(b"\x00\x01" * 4096)
        device.send(Ping())
        device.recv()  # the pong proves the audio was accepted before we drop the socket

    # The connection is gone, and with it the audio it was assembling.
    assert len(registry) == 0

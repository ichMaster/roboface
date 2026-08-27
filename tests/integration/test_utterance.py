"""A streamed utterance, assembled by the server.

The claim under test is that the audio arriving as many small binary frames is reassembled in
order and without loss. A byte count alone would not show that -- reordering and duplication both
preserve it -- so these compare the digest the server logs against one computed here.
"""

from __future__ import annotations

import hashlib
from collections.abc import Callable
from typing import Any

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.protocol import AUDIO_SAMPLE_RATE, Ping, Pong


def speech(seconds: float = 1.0) -> bytes:
    """A recognisable PCM16 pattern -- not silence, so a dropped or repeated frame shows up."""
    samples = int(AUDIO_SAMPLE_RATE * seconds)
    return b"".join(((index * 37) % 65536).to_bytes(2, "little") for index in range(samples))


def test_a_streamed_utterance_is_assembled_in_order_and_intact(
    log_lines: Callable[[], list[dict[str, Any]]],
) -> None:
    pcm = speech(1.0)
    with connect(create_app()) as device:
        device.hello()
        device.utterance(pcm)
        device.send(Ping())
        assert isinstance(device.recv(), Pong), "the window should have closed without an error"

    stops = [line for line in log_lines() if line["event"] == "listen.stop"]
    assert len(stops) == 1
    assert stops[0]["bytes"] == len(pcm)
    assert stops[0]["digest"] == hashlib.sha256(pcm).hexdigest()[:16], (
        "the assembled audio differs from what was sent -- reordered, dropped or duplicated"
    )


def test_a_second_of_speech_arrives_as_many_frames_not_one(
    log_lines: Callable[[], list[dict[str, Any]]],
) -> None:
    # The whole point of the phase: audio arrives *during* speech. One large frame would satisfy
    # every other assertion here and would mean the device buffered the utterance and uploaded it.
    with connect(create_app()) as device:
        device.hello()
        device.utterance(speech(1.0))

    stop = next(line for line in log_lines() if line["event"] == "listen.stop")
    assert stop["frames"] >= 40, f"a second of audio arrived as {stop['frames']} frames"


def test_two_utterances_on_one_connection_do_not_bleed(
    log_lines: Callable[[], list[dict[str, Any]]],
) -> None:
    # The second must not carry the first's tail. `_end_utterance` releasing the buffer is what
    # makes that true, and a leak here would show up as an ASR transcript with a stale prefix.
    first, second = speech(0.2), speech(0.3)
    with connect(create_app()) as device:
        device.hello()
        device.utterance(first)
        device.utterance(second)

    stops = [line for line in log_lines() if line["event"] == "listen.stop"]
    assert len(stops) == 2
    assert stops[0]["digest"] == hashlib.sha256(first).hexdigest()[:16]
    assert stops[1]["digest"] == hashlib.sha256(second).hexdigest()[:16]


def test_an_empty_utterance_is_valid(log_lines: Callable[[], list[dict[str, Any]]]) -> None:
    # A window opened and closed with nothing in it. Not an error -- v1.2's PTT can produce one on
    # a very short hold, and v1.3 must be able to see zero bytes rather than a fault.
    with connect(create_app()) as device:
        device.hello()
        device.utterance(b"")

    stop = next(line for line in log_lines() if line["event"] == "listen.stop")
    assert stop["bytes"] == 0
    assert stop["frames"] == 0

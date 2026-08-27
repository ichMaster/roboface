"""The turn lifecycle: what ends an utterance, and what a duplicate ending must not do.

§v1.4 moves the primary end-of-utterance decision from the device to the recogniser. That is a
change to the documented lifecycle in ARCHITECTURE.md §Turn lifecycle, so it is pinned here rather
than left to the integration tests -- the property is about *who decides*, and an integration test
that happens to pass would not say which end decided.
"""

from __future__ import annotations

from typing import Any

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import Asr, ErrorFrame, ListenStart, ListenStop, Reply
from roboface_server.providers.mock import MockASRProvider, MockLLMProvider, MockTTSProvider

FRAME = b"\x01\x02" * 320


def build(**kwargs: Any) -> Any:
    orchestrator = Orchestrator(
        provider=kwargs.pop("llm", MockLLMProvider(deltas=("Добре.",))),
        tts=kwargs.pop("tts", MockTTSProvider()),
        asr=kwargs.pop("asr", MockASRProvider(settle_after=2)),
        **kwargs,
    )
    return orchestrator, create_app(responder=orchestrator)


def drain(device: Any) -> list[Any]:
    """Every JSON frame of one turn. `collect_turn` interleaves the binary audio; the lifecycle
    questions here are about frames, so the payloads are dropped rather than reasoned about."""
    return [event for event in device.collect_turn().events if not isinstance(event, bytes)]


def test_the_recogniser_ends_the_utterance_without_listen_stop() -> None:
    # The claim §v1.4 rests on: the recogniser hears the same silence the person made and calls it
    # while the audio is still arriving. The device sends no `listen_stop` here at all.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        frames = drain(device)

    assert any(isinstance(f, Asr) for f in frames), "the transcript never arrived"
    assert any(isinstance(f, Reply) and f.text for f in frames), "the turn never ran"


def test_a_late_listen_stop_is_not_a_second_turn() -> None:
    # Both ends are correct and simply noticed the silence at different moments: the recogniser
    # first, the device's end-pause a little later. The device must not be told it is wrong, and
    # the person must not be answered twice.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        first = drain(device)

        device.send(ListenStop())
        device.send(ListenStart())  # a probe: the connection must still be usable
        # Four frames, not two: the settle is noticed on the frame *after* the one that caused it,
        # which is how the router avoids polling. An utterance whose last frame settles is closed
        # by `listen_stop` instead -- covered by the backstop test below.
        for _ in range(4):
            device.send_binary(FRAME)
        second = drain(device)

    assert not any(isinstance(f, ErrorFrame) for f in first + second), (
        "a listen_stop for an utterance the recogniser already ended was treated as an error"
    )
    assert sum(1 for f in first if isinstance(f, Asr)) == 1
    assert sum(1 for f in second if isinstance(f, Asr)) == 1, "the next utterance was lost"


def test_listen_stop_still_ends_an_utterance_the_recogniser_never_settled() -> None:
    # The backstop. With no mid-window settling the device's end-pause is what closes the window,
    # exactly as in v1.3 -- so a recogniser that never endpoints does not strand the turn.
    _, app = build(asr=MockASRProvider())
    with connect(app) as device:
        device.hello()
        device.send(ListenStart())
        for _ in range(4):
            device.send_binary(FRAME)
        device.send(ListenStop())
        frames = drain(device)

    assert any(isinstance(f, Asr) for f in frames)
    assert any(isinstance(f, Reply) and f.text for f in frames)


def test_listen_stop_with_no_window_is_still_an_error() -> None:
    # The tolerance added for the settled case must not become a general amnesty: a `listen_stop`
    # with no window and nothing settled is still the device and the server disagreeing about
    # state, which is worth surfacing.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.send(ListenStop())
        assert isinstance(device.recv(), ErrorFrame)

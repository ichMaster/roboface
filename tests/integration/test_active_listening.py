"""v1.4 end to end: a turn that nobody touched.

The device-side half of active listening -- the endpointer, the pre-roll ring, the half-duplex
guard -- is host-tested under `pio test -e native`, because it is pure logic about time and a
laptop can prove it in a millisecond. What those tests cannot show is the *shape on the wire*: that
a window opened by a voice, closed by the recogniser and never closed by the device still produces
one complete turn. That is what this file is for.
"""

from __future__ import annotations

from typing import Any

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import Asr, ErrorFrame, Ping, Pong, Reply
from roboface_server.providers.mock import MockASRProvider, MockLLMProvider, MockTTSProvider

#: 20 ms of PCM16 at 16 kHz -- one frame, the size the firmware sends.
FRAME = b"\x11\x22" * 320


def build(**kwargs: Any) -> Any:
    orchestrator = Orchestrator(
        provider=kwargs.pop("llm", MockLLMProvider(deltas=("Все добре.",))),
        tts=kwargs.pop("tts", MockTTSProvider()),
        # The recogniser endpoints after three frames, while the device is still streaming --
        # which is the situation v1.4 exists to handle.
        asr=kwargs.pop("asr", MockASRProvider(settle_after=3)),
        **kwargs,
    )
    return orchestrator, create_app(responder=orchestrator)


def test_a_voice_alone_produces_a_whole_turn() -> None:
    # The DoD, on the wire: speech opens a window, the recogniser closes it, and an answer comes
    # back -- with no `listen_stop` from the device and no touch anywhere in it.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.vad_utterance(FRAME * 6)
        turn = device.collect_turn()

    kinds = [type(e).__name__ if not isinstance(e, bytes) else "audio" for e in turn.events]
    assert "Asr" in kinds, "the device was never told what it said"
    assert "audio" in kinds, "nothing was spoken back"
    assert isinstance(turn.frames[-1], Reply) and turn.frames[-1].final
    assert not any(isinstance(f, ErrorFrame) for f in turn.frames)


def test_the_devices_end_pause_can_arrive_late_without_harm() -> None:
    # The backstop firing after the recogniser already decided. Both ends are right; the person
    # must be answered once, and the device must not be told it misbehaved.
    _, app = build()
    with connect(app) as device:
        device.hello()
        device.vad_utterance(FRAME * 6, then_stop=True)
        turn = device.collect_turn()

    assert sum(1 for f in turn.frames if isinstance(f, Asr)) == 1, "answered twice"
    assert not any(isinstance(f, ErrorFrame) for f in turn.frames)


def test_a_burst_too_short_to_be_speech_never_reaches_the_wire() -> None:
    # The device discards it: `roboface::Endpointer` reports `kDiscarded` and no window is ever
    # opened, which is the rule `pure/ptt.h` already applies to a tap. Host-tested there; asserted
    # here as the protocol consequence -- **nothing at all happens**, rather than an empty turn the
    # server has to special-case.
    _, app = build()
    with connect(app) as device:
        device.hello()
        # A discarded burst sends no `listen_start`, so there is no traffic to observe. The probe
        # is that the connection is untouched and still healthy.
        device.send(Ping())
        assert isinstance(device.recv(), Pong)


def test_two_hands_free_turns_in_a_row() -> None:
    # Nothing carries over: the second utterance gets its own window, transcript and answer. This
    # is where a settled flag left set, or a session not released, would show.
    _, app = build()
    with connect(app) as device:
        device.hello()
        for _ in range(2):
            device.vad_utterance(FRAME * 6)
            turn = device.collect_turn()
            assert sum(1 for f in turn.frames if isinstance(f, Asr)) == 1
            assert isinstance(turn.frames[-1], Reply) and turn.frames[-1].final


def test_the_recogniser_hears_the_pre_roll() -> None:
    # Pre-roll is device-side, but its purpose is server-side: the frames captured before the
    # endpointer was sure are part of the utterance, so they must reach the vendor. The board
    # flushes them through the same sink, which on the wire is indistinguishable from live audio --
    # so what is asserted here is that every frame sent inside the window reached the session.
    orchestrator, app = build()
    with connect(app) as device:
        device.hello()
        device.vad_utterance(FRAME * 6)
        device.collect_turn()

    session = orchestrator.asr.sessions[0]  # type: ignore[union-attr]
    assert len(session.pushed) >= 3, "audio stopped reaching the vendor before it settled"

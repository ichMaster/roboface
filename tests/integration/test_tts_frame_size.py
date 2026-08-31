"""v2.6.2 — the server never hands the device a frame it cannot swallow.

**Found on hardware, and not fixable on the device.** The firmware's WebSocket client reassembles a
whole binary frame before calling its handler, and the handler is where the speaker gets fed. So an
oversized frame is a window during which the device is *not running* — measured at 173 ms, against a
playback buffer of 64 ms. The reply breaks up, and no amount of care in the firmware can help,
because the firmware is not executing during the gap.

That makes the frame size the server's responsibility. The provider yields whatever the network
handed it — `aiter_bytes()` with no size — so the bound belongs at the one place that knows what the
device can swallow.
"""

from __future__ import annotations

from fake_device import connect
from roboface_server.app import create_app
from roboface_server.orchestrator import Orchestrator
from roboface_server.protocol import MAX_TTS_FRAME_BYTES, TextIn
from roboface_server.providers.mock import MockLLMProvider, MockTTSProvider


def test_no_audio_frame_exceeds_the_cap() -> None:
    # A TTS chunk far larger than the cap, as ElevenLabs really delivers over a fast link.
    oversized = b"\x00\x01" * 20_000  # 40 KB in one provider chunk
    app = create_app(
        responder=Orchestrator(
            provider=MockLLMProvider(deltas=("Добре.",)),
            tts=MockTTSProvider(chunks=(oversized,)),
        )
    )

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert turn.audio, "the turn produced no audio at all"
    for frame in turn.audio:
        assert len(frame) <= MAX_TTS_FRAME_BYTES, f"a {len(frame)} byte frame reached the device"


def test_the_audio_arrives_whole_and_in_order() -> None:
    """Splitting must not lose or reorder a byte.

    PCM16 is two bytes per sample, so a split at an odd offset would swap the halves of a sample and
    arrive as a click — which is why the cap is even, and why this asserts the joined stream rather
    than the frame count.
    """
    original = bytes((i * 7 + 3) % 256 for i in range(30_000))
    app = create_app(
        responder=Orchestrator(
            provider=MockLLMProvider(deltas=("Добре.",)),
            tts=MockTTSProvider(chunks=(original,)),
        )
    )

    with connect(app) as device:
        device.hello()
        device.send(TextIn(text="привіт"))
        turn = device.collect_turn()

    assert b"".join(turn.audio) == original
    assert MAX_TTS_FRAME_BYTES % 2 == 0, "an odd cap would split a sample in half"

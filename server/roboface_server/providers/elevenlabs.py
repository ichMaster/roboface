"""ElevenLabs, streamed as PCM16.

The real :class:`~roboface_server.providers.base.TTSProvider`. ARCHITECTURE §Audio pipeline: TTS
yields the device's playback format directly, so **nothing decodes anywhere** — `pcm_16000` is
16 kHz mono PCM16, which is exactly `AUDIO_FMT` in ``protocol.py``, which is exactly what the
speaker plays. Asking for MP3 would put a decoder on an ESP32 in the middle of a latency budget.

``httpx`` is imported **inside** the call, as ``providers/gemini.py`` imports the Gemini SDK: a
package-level import would make every test in the repository depend on a wheel that only the real
adapter needs.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from typing import Final

from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import ProviderError

#: The streaming endpoint. `/stream` rather than the plain synthesis route is the whole point: the
#: plain one returns the finished audio, which would mean waiting for the entire phrase before the
#: first sample could leave.
API_BASE: Final = "https://api.elevenlabs.io/v1/text-to-speech"

#: Whole-request ceiling. Generous, because a long phrase legitimately takes a while and the
#: *latency* that matters is bounded by the orchestrator's first-audio budget on the first chunk,
#: not by this.
DEFAULT_TIMEOUT_S: Final = 30.0

#: Same policy as the Gemini adapter, and for the same reason (v0.2 review, finding 3): hint only
#: the statuses whose meaning is unambiguous *from the device's point of view*, and never one that
#: blames the device for a vendor problem. DEVICE_UI renders a fault as its enumerated code, so
#: mapping a vendor 503 to ``server_unreachable`` would print "No server" on a device that is
#: connected to its server and being told so over that very connection.
#:
#: Everything unhinted becomes ``tts_failed``, which is true and actionable: speech did not happen.
#: The real cause is logged server-side, where the person who can fix a bad key will look.
_STATUS_HINTS: Final[dict[int, ErrorCode]] = {
    # Rate limiting is rate limiting wherever it happened, and backing off is the right response.
    429: ErrorCode.RATE_LIMITED,
}


class ElevenLabsProvider:
    """Streams PCM16 chunks from ElevenLabs as they are generated."""

    def __init__(
        self,
        api_key: str,
        voice_id: str,
        model: str,
        output_format: str,
        *,
        timeout_s: float = DEFAULT_TIMEOUT_S,
    ) -> None:
        if not api_key:
            raise ProviderError("ELEVENLABS_API_KEY is not set", ErrorCode.TTS_FAILED)
        if not voice_id:
            raise ProviderError("ELEVENLABS_VOICE_ID is not set", ErrorCode.TTS_FAILED)
        self._api_key = api_key
        self._voice_id = voice_id
        self._model = model
        self._output_format = output_format
        self._timeout_s = timeout_s

    def synthesize(self, text: str) -> AsyncIterator[bytes]:
        """Yield PCM16 chunks as they arrive. Not ``async def`` — see the seam."""
        return self._synthesize(text)

    async def _synthesize(self, text: str) -> AsyncIterator[bytes]:
        try:
            import httpx
        except ImportError as exc:  # pragma: no cover -- environment problem, not a code path
            raise ProviderError(
                "httpx is not installed; the ElevenLabs adapter needs it", ErrorCode.TTS_FAILED
            ) from exc

        url = f"{API_BASE}/{self._voice_id}/stream"
        headers = {"xi-api-key": self._api_key, "content-type": "application/json"}
        params = {"output_format": self._output_format}
        payload = {"text": text, "model_id": self._model}

        # PCM16 is two bytes per sample and the transport splits wherever it likes, so a chunk can
        # end mid-sample. Forwarding that would shift every following sample by a byte -- the audio
        # does not glitch once, it turns to noise for the rest of the phrase. The odd byte is
        # carried into the next chunk instead.
        remainder = b""
        try:
            async with (
                httpx.AsyncClient(timeout=self._timeout_s) as client,
                client.stream(
                    "POST", url, headers=headers, params=params, json=payload
                ) as response,
            ):
                if response.status_code != 200:
                    body = await response.aread()
                    raise self._translate_status(response.status_code, body)
                async for chunk in response.aiter_bytes():
                    if not chunk:
                        continue
                    data = remainder + chunk
                    cut = len(data) - (len(data) % 2)
                    remainder = data[cut:]
                    if cut:
                        yield data[:cut]
        except ProviderError:
            raise
        except Exception as exc:
            raise ProviderError(
                f"elevenlabs synthesis failed: {exc}", ErrorCode.TTS_FAILED
            ) from exc

        # A single trailing byte is half a sample. There is nothing to play it with, and padding it
        # would invent a sample the model never produced, so it is dropped.

    @staticmethod
    def _translate_status(status: int, body: bytes) -> ProviderError:
        """Turn a non-200 into the server's vocabulary."""
        detail = body[:200].decode("utf-8", "replace").strip()
        code = _STATUS_HINTS.get(status, ErrorCode.TTS_FAILED)
        return ProviderError(f"elevenlabs returned {status}: {detail}", code)

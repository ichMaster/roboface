"""The deterministic provider every test uses by default.

ARCHITECTURE §Testing and CI: "mock Gemini/Deepgram/ElevenLabs return canned streams. CI never
makes a paid call." This is the LLM half of that, and it is the provider the orchestrator is
developed against — the Gemini adapter is written in parallel and neither waits for the other.

It gives RF-008 and RF-009 the three levers a streaming turn needs to be tested honestly:

* **several deltas**, so a buffered implementation cannot pass a test a streaming one passes;
* a **delay**, placed before a chosen delta, so a stalled first token and a merely slow middle
  can be told apart — the distinction the first-token budget exists to make;
* a **failure at a chosen index**, so a mid-stream death can be tested separately from a
  refusal to start.

Determinism is the point. A mock that varies is a suite that flakes, and a flaky suite is one
nobody reads the failures of.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Sequence

from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import Message, ProviderError

#: What a reply looks like when a test does not care about the words. Several fragments, not
#: one string: a single-delta mock would let a buffering implementation pass every test here.
DEFAULT_DELTAS: tuple[str, ...] = ("Привіт", "! ", "Я ", "тут ", "і ", "слухаю ", "тебе.")


class MockLLMProvider:
    """A canned :class:`~roboface_server.providers.base.LLMProvider`.

    Records what it was asked, so a test can assert the system prompt and history actually
    reached the provider rather than trusting that they did.
    """

    def __init__(
        self,
        deltas: Sequence[str] | None = None,
        *,
        delay_s: float = 0.0,
        delay_before_index: int = 0,
        fail_at_index: int | None = None,
        error: ProviderError | None = None,
    ) -> None:
        self.deltas: tuple[str, ...] = tuple(DEFAULT_DELTAS if deltas is None else deltas)
        self.delay_s = delay_s
        #: Which delta the delay precedes. 0 stalls the *first* token (the budget should
        #: fire); a higher index stalls mid-stream (it should not).
        self.delay_before_index = delay_before_index
        self.fail_at_index = fail_at_index
        self.error = error if error is not None else ProviderError("mock provider failure")

        #: What the last call was given. Assertions read these instead of assuming.
        self.calls: list[tuple[str, tuple[Message, ...]]] = []

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[str]:
        self.calls.append((system, tuple(messages)))
        return self._stream()

    async def _stream(self) -> AsyncIterator[str]:
        for index, delta in enumerate(self.deltas):
            if self.fail_at_index is not None and index == self.fail_at_index:
                raise self.error
            if self.delay_s and index == self.delay_before_index:
                await asyncio.sleep(self.delay_s)
            yield delta

        # A failure index past the end means "fail after the last delta" -- a stream that ends
        # in an error rather than a completion, which is a different case from failing part of
        # the way through and is worth being able to express.
        if self.fail_at_index is not None and self.fail_at_index >= len(self.deltas):
            raise self.error


class SilentLLMProvider:
    """Yields nothing at all, successfully.

    Not a failure: ARCHITECTURE treats "silence that transcribes to nothing" as a clean end to
    a turn, and the model declining to say anything is the same shape. RF-009 asserts that this
    ends the turn rather than erroring, which is easy to get wrong in the direction of a
    spurious ``llm_failed``.
    """

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[str]:
        return self._stream()

    async def _stream(self) -> AsyncIterator[str]:
        return
        yield  # pragma: no cover -- unreachable; makes this an async generator


#: Four chunks of silence, and **four is the point**: a single-chunk mock lets a consumer that
#: accumulates before sending pass every test, and accumulating is exactly what this phase removes.
#: Even byte counts, because PCM16 samples are two bytes and an odd split would be a malformed
#: sample rather than a smaller one.
DEFAULT_CHUNKS: tuple[bytes, ...] = (
    b"\x00\x01" * 160,
    b"\x00\x02" * 160,
    b"\x00\x03" * 160,
    b"\x00\x04" * 160,
)


class MockTTSProvider:
    """A canned :class:`~roboface_server.providers.base.TTSProvider`.

    Records the text it was asked to speak, so a test can assert that the *phrase* reached the
    provider — not the whole reply, and not a half-word — rather than trusting that it did.
    """

    def __init__(
        self,
        chunks: Sequence[bytes] | None = None,
        *,
        delay_s: float = 0.0,
        delay_before_index: int = 0,
        fail_at_index: int | None = None,
        error: ProviderError | None = None,
    ) -> None:
        self.chunks: tuple[bytes, ...] = tuple(DEFAULT_CHUNKS if chunks is None else chunks)
        self.delay_s = delay_s
        #: Which chunk the delay precedes. 0 stalls the *first* one (the first-audio budget should
        #: fire); a higher index stalls mid-phrase (it should not).
        self.delay_before_index = delay_before_index
        self.fail_at_index = fail_at_index
        self.error = (
            error if error is not None else ProviderError("mock tts failure", ErrorCode.TTS_FAILED)
        )

        #: Every phrase this provider was asked to speak, in order.
        self.calls: list[str] = []

    def synthesize(self, text: str) -> AsyncIterator[bytes]:
        self.calls.append(text)
        return self._synthesize()

    async def _synthesize(self) -> AsyncIterator[bytes]:
        for index, chunk in enumerate(self.chunks):
            if self.fail_at_index is not None and index == self.fail_at_index:
                raise self.error
            if self.delay_s and index == self.delay_before_index:
                await asyncio.sleep(self.delay_s)
            yield chunk

        if self.fail_at_index is not None and self.fail_at_index >= len(self.chunks):
            raise self.error

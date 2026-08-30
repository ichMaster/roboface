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

from roboface_server.emotion import ModelReport
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import (
    ASRChunk,
    LLMEvent,
    Message,
    ProviderError,
    ReplyText,
)

#: What a reply looks like when a test does not care about the words. Several fragments, not
#: one string: a single-delta mock would let a buffering implementation pass every test here.
DEFAULT_DELTAS: tuple[str, ...] = ("Привіт", "! ", "Я ", "тут ", "і ", "слухаю ", "тебе.")

#: What the mock reports about itself unless a test says otherwise. `joy` rather than `neutral`,
#: deliberately: a default that happened to match the fallback would let a test pass whether the
#: report was read or ignored.
DEFAULT_REPORT = ModelReport(emotion="joy", intensity=0.7)

#: Distinguishes "the caller said nothing about the report" from "the caller asked for no report".
#: `None` is a meaningful value here -- a model that answers without reporting is a real case -- so
#: it cannot also serve as the absent-argument marker.
_NO_REPORT: ModelReport = ModelReport(emotion="<unset>", intensity=-1.0)


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
        report: ModelReport | None = _NO_REPORT,
    ) -> None:
        #: What the model says about its own state, yielded **before** the first delta -- the
        #: order the real provider guarantees, so a test that depends on it is testing the
        #: contract rather than an accident of this mock.
        #:
        #: The default is a plain, valid report: most tests want a turn that behaves like a turn.
        #: Passing ``report=None`` gets a stream that reports nothing, which is a case the real
        #: model produces too and which the emotion engine must survive.
        self.report = DEFAULT_REPORT if report is _NO_REPORT else report
        self.deltas: tuple[str, ...] = tuple(DEFAULT_DELTAS if deltas is None else deltas)
        self.delay_s = delay_s
        #: Which delta the delay precedes. 0 stalls the *first* token (the budget should
        #: fire); a higher index stalls mid-stream (it should not).
        self.delay_before_index = delay_before_index
        self.fail_at_index = fail_at_index
        self.error = error if error is not None else ProviderError("mock provider failure")

        #: What the last call was given. Assertions read these instead of assuming.
        self.calls: list[tuple[str, tuple[Message, ...]]] = []

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[LLMEvent]:
        self.calls.append((system, tuple(messages)))
        return self._stream()

    async def _stream(self) -> AsyncIterator[LLMEvent]:
        # Before the first delta and before the first delay: the report is what the real schema
        # produces first, and a mock that emitted it later would let a first-token-budget test
        # pass for the wrong reason.
        if self.report is not None:
            yield self.report

        for index, delta in enumerate(self.deltas):
            if self.fail_at_index is not None and index == self.fail_at_index:
                raise self.error
            if self.delay_s and index == self.delay_before_index:
                await asyncio.sleep(self.delay_s)
            yield ReplyText(text=delta)

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

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[LLMEvent]:
        return self._stream()

    async def _stream(self) -> AsyncIterator[LLMEvent]:
        # No report either. Silence is silence: a model that says nothing has said nothing about
        # how it feels, and inventing a `neutral` report here would make this provider a weaker
        # test than it is -- the emotion engine's no-report fallback would never be exercised.
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


#: A scripted Deepgram-shaped exchange: interims that get revised, then a punctuated final. The
#: revision matters -- a mock whose interims only grew would let a tracker that *accumulates*
#: interims pass, and accumulating them produces "прив прив привіт" on the screen.
DEFAULT_ASR_SCRIPT: tuple[ASRChunk, ...] = (
    ASRChunk(text="прив", is_final=False),
    ASRChunk(text="привіт", is_final=False),
    ASRChunk(text="привіт як", is_final=False),
    ASRChunk(text="Привіт, як справи?", is_final=True),
)


class MockASRSession:
    """Replays a script, and records the audio it was fed."""

    def __init__(
        self,
        script: Sequence[ASRChunk],
        error: ProviderError | None = None,
        settle_after: int | None = None,
    ) -> None:
        self._script = tuple(script)
        self._error = error
        #: How many pushed frames before the script is released, modelling the recogniser's own
        #: endpointing (§v1.4). ``None`` releases it only at ``finish``, which is v1.3's shape and
        #: what most tests want.
        #:
        #: This exists because the obvious mock -- yield the script the moment anyone iterates --
        #: models a recogniser that has decided before it has heard anything. Under v1.3 that was
        #: invisible: nothing read the result until ``finish``. v1.4 reads it *during* the window,
        #: and an always-ready mock would end every utterance on its first frame.
        self._settle_after = settle_after
        self._ready = asyncio.Event()
        #: Every frame pushed, in order. A test asserts audio reached the vendor *during* the
        #: window rather than trusting that it did.
        self.pushed: list[bytes] = []
        self.finished = False
        self.closed = False

    async def push(self, audio: bytes) -> None:
        self.pushed.append(audio)
        if self._settle_after is not None and len(self.pushed) >= self._settle_after:
            self._ready.set()

    async def finish(self) -> None:
        self.finished = True
        self._ready.set()

    def results(self) -> AsyncIterator[ASRChunk]:
        return self._results()

    async def _results(self) -> AsyncIterator[ASRChunk]:
        if self._error is not None:
            raise self._error
        await self._ready.wait()
        for chunk in self._script:
            yield chunk

    async def close(self) -> None:
        self.closed = True


class MockASRProvider:
    """A canned :class:`~roboface_server.providers.base.ASRProvider`."""

    def __init__(
        self,
        script: Sequence[ASRChunk] | None = None,
        *,
        error: ProviderError | None = None,
        settle_after: int | None = None,
    ) -> None:
        self.script = tuple(DEFAULT_ASR_SCRIPT if script is None else script)
        self.error = error
        #: See :class:`MockASRSession`. ``None`` means the recogniser settles only when the audio
        #: ends; a number models it endpointing mid-window, after that many frames.
        self.settle_after = settle_after
        #: Every session opened, so a test can inspect what was pushed after the turn.
        self.sessions: list[MockASRSession] = []

    def open(self) -> MockASRSession:
        session = MockASRSession(self.script, self.error, self.settle_after)
        self.sessions.append(session)
        return session

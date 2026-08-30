"""The provider seams — one per external service, each with a mock.

v0.2 introduces the first: :class:`LLMProvider`. ASR and TTS join it in v1 behind the same
discipline (ARCHITECTURE §Providers and seams). What a seam hides is a *vendor*; what it
exposes is a *shape*, and here the shape is the load-bearing part.

**`stream` returns an async iterator, not a string.** That is the streaming guarantee written
into the type. ARCHITECTURE §Streaming is the architecture, not an optimisation requires that
"the reply is never accumulated and sent whole", and a signature able to return a whole reply
is a signature that will eventually be handed one — by a well-meaning adapter that finds
collecting easier, and by nothing in the type checker to stop it. From v1 the TTS stage starts
synthesizing on the first completed phrase; it cannot do that if the reply arrives at the end.

Nothing here imports a vendor SDK. `mock.py` needs none at all, and `gemini.py` (RF-010) is the
only module that does — so the contract test, and the whole default suite, run with no API key
and no network.
"""

from __future__ import annotations

from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass
from typing import Literal, Protocol, runtime_checkable

from roboface_server.emotion import ModelReport
from roboface_server.protocol import ErrorCode

#: Who said a thing. `model` rather than `assistant`: it is the vendor-neutral word, and the
#: one Gemini itself uses.
Role = Literal["user", "model"]


@dataclass(frozen=True, slots=True)
class Message:
    """One turn of conversation history.

    In memory only. ARCHITECTURE §Data model persists nothing before v4, where
    ``SessionMessage`` gains a SQLite table and an emotion alongside the text; this is
    deliberately the smaller thing until then.
    """

    role: Role
    text: str


class ProviderError(Exception):
    """A provider failed, in the vocabulary the rest of the server speaks.

    The orchestrator must never see a vendor exception type — translating them is the seam's
    entire job (ARCHITECTURE §Providers and seams).

    ``code`` is **optional** on purpose. An adapter that can tell it was rate-limited should
    say so; one that only knows the call failed should not have to guess, because a guessed
    ``rate_limited`` would send the device a face and a retry policy chosen for a reason that
    was never true. RF-009 maps a missing hint to ``llm_failed`` in one place, rather than
    letting every adapter invent its own default.
    """

    def __init__(self, message: str, code: ErrorCode | None = None) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True, slots=True)
class ReplyText:
    """One piece of the model's answer, ready to send."""

    text: str


#: What a chat stream can produce. From v2.2 a reply is not only text: the model reports its own
#: emotional state alongside it, and the two arrive **interleaved on one connection**.
#:
#: A union rather than a second method, and the reason is latency rather than taste. A separate
#: call asking "how do you feel about what you just said?" would cost a second round trip on every
#: turn — and by then the device has already spoken the answer with the wrong face.
#:
#: :class:`~roboface_server.emotion.ModelReport` is reused rather than re-declared: the provider
#: produces exactly what the emotion engine consumes, and two structurally identical types would
#: drift the first time one of them gained a field.
LLMEvent = ReplyText | ModelReport


@runtime_checkable
class LLMProvider(Protocol):
    """Chat completion, streamed.

    Exactly one real implementation exists and exactly one ever will — Gemini 2.5 Flash with
    ``thinkingBudget: 0`` (MISSION §Principles, ARCHITECTURE §Model policy). The seam is here
    to keep the vendor out of the orchestrator and to make the suite free, not to leave room
    for a second chat vendor: that is a non-goal, not a backlog item.
    """

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[LLMEvent]:
        """Yield reply deltas and the model's self-report as they arrive.

        Not ``async def``: the method itself returns the iterator, so a caller can await the
        *first* event under its own budget (ARCHITECTURE §Budgets and abort semantics) without
        awaiting the whole call first. An ``async def`` returning an iterator would defer that
        choice to the implementation, which is exactly where it must not live.

        **Order is part of the contract**: the report, when there is one, comes before the text it
        describes. The device should change expression as it begins to speak, not after — and the
        only way to guarantee that is for the report to be the first thing the model produces,
        which is why the response schema puts it first (see `gemini.py`).

        A stream may carry no report at all. That is not a failure: the model answered, which is
        what a turn is for, and the emotion engine falls back to `neutral`.
        """
        ...


@runtime_checkable
class TTSProvider(Protocol):
    """Speech synthesis, streamed.

    The second vendor seam, and the reason MISSION's "chat is Gemini and only Gemini" is not a
    statement about the whole system: **speech is not chat**. ElevenLabs sits here, Deepgram sits
    behind ``ASRProvider`` from v1.3, and swapping either is a provider change rather than an
    architecture one.

    Chunks are **PCM16, 16 kHz, mono** — the device's playback format, which is why the real
    adapter asks ElevenLabs for ``pcm_16000`` rather than for MP3. Nothing decodes anywhere: what
    arrives from the vendor is what goes on the wire and what the speaker plays.
    """

    def synthesize(self, text: str) -> AsyncIterator[bytes]:
        """Yield PCM16 chunks as they are generated.

        Not ``async def``, for the same reason :meth:`LLMProvider.stream` is not: the caller must
        hold the iterator before awaiting anything, so it can put its *first-audio* budget on the
        first chunk alone (ARCHITECTURE §Budgets and abort semantics). A stalled vendor then fails
        fast while a slow-but-flowing one is never cut off mid-phrase.

        Implementations must not accumulate. A provider that gathered the whole phrase before
        yielding would satisfy every type in this signature and defeat the entire phase: the point
        of v1.1 is that the mouth opens before the sentence exists.
        """
        ...


@dataclass(frozen=True, slots=True)
class ASRChunk:
    """One recognition result. ``is_final`` marks a stable transcript rather than a guess.

    Deepgram emits interims constantly and revises them; a final is the vendor saying it will not
    change its mind about *this* span. It is **not** a claim that the person has stopped talking --
    that judgement is `utterance.py`'s, and conflating the two is what makes a character interrupt
    someone who paused for breath.
    """

    text: str
    is_final: bool
    #: How sure the vendor is, 0..1. Optional in the seam -- a provider that does not report it
    #: leaves it at 0.0 -- and used for one decision: which draft to keep when the final arrives
    #: empty. Deepgram does that regularly, publishing a good transcript as an interim and then
    #: closing the span with nothing, and picking the surviving draft by *length* would prefer a
    #: long mishearing over a short correct one.
    confidence: float = 0.0


@runtime_checkable
class ASRSession(Protocol):
    """One recognition session: audio in, transcripts out, at the same time.

    A session rather than a call, because recognition has to run **during** speech. A seam shaped
    as ``transcribe(audio) -> str`` could only start once the audio was complete, which is the
    ~1.4 s this phase exists to remove -- and it is paid at the worst possible moment, after the
    person has stopped talking and is waiting.
    """

    async def push(self, audio: bytes) -> None:
        """Feed captured PCM16 into the session. Safe to call while chunks are being read."""
        ...

    async def finish(self) -> None:
        """No more audio is coming. The vendor may still emit finals after this."""
        ...

    def results(self) -> AsyncIterator[ASRChunk]:
        """Transcripts as they are recognised. Not ``async def``, for the usual reason."""
        ...

    async def close(self) -> None:
        """Release the session, whether or not it finished. Idempotent."""
        ...


@runtime_checkable
class ASRProvider(Protocol):
    """Speech recognition, streamed.

    The third and last vendor seam: chat is Gemini (MISSION), speech is Deepgram in and ElevenLabs
    out, and none of the three is the same seam as another.
    """

    def open(self) -> ASRSession:
        """Begin a session. Not ``async def``: the caller holds it before awaiting anything, so it
        can push the first audio frame under its own budget."""
        ...

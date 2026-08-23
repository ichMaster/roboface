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


@runtime_checkable
class LLMProvider(Protocol):
    """Chat completion, streamed.

    Exactly one real implementation exists and exactly one ever will — Gemini 2.5 Flash with
    ``thinkingBudget: 0`` (MISSION §Principles, ARCHITECTURE §Model policy). The seam is here
    to keep the vendor out of the orchestrator and to make the suite free, not to leave room
    for a second chat vendor: that is a non-goal, not a backlog item.
    """

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[str]:
        """Yield reply deltas as they arrive.

        Not ``async def``: the method itself returns the iterator, so a caller can await the
        *first* delta under its own budget (ARCHITECTURE §Budgets and abort semantics) without
        awaiting the whole call first. An ``async def`` returning an iterator would defer that
        choice to the implementation, which is exactly where it must not live.
        """
        ...

"""Runs a turn, and streams it.

ARCHITECTURE §Streaming is the architecture, not an optimisation is the specification for this
module, and one sentence of it is the whole design: *"Gemini token deltas leave as individual
``reply`` frames as they arrive; the reply is never accumulated and sent whole."*

That is not a latency tweak to revisit later. From v1 the TTS stage synthesizes each completed
phrase the moment it exists, and the device plays audio while the model is still generating; the
property v1's tests assert is that **the first audio leaves before the reply is finished**. A
turn that accumulated its reply here would make that impossible, and the discovery would come in
v1 with a rewrite as the fix.

**The budget is awaited on the first delta only.** ARCHITECTURE §Budgets and abort semantics:
*"awaited on the stage's first output so a stalled provider fails fast while a slow-but-flowing
one is never cut off mid-reply."* A single timeout around the whole stream would truncate a long,
healthy answer at exactly the moment it was going well, so the distinction between "has not
started" and "is taking a while" is a requirement rather than a nicety.

History is **in memory, per session**. ARCHITECTURE §Data model persists nothing before v4; the
rolling window and the SQLite table arrive with it.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from typing import Final

from roboface_server.logging import chars, log
from roboface_server.prompt import build_system_prompt
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import LLMProvider, Message, ProviderError

#: How long the model has to produce its **first** delta. ARCHITECTURE §Budgets and abort
#: semantics puts the LLM's first-token budget at ~8 s. Nothing bounds the deltas after it --
#: a model mid-sentence is working, and cutting it off is worse than waiting.
DEFAULT_FIRST_TOKEN_BUDGET_S: Final = 8.0


class TurnAborted(Exception):
    """A turn ended without a reply, with the enumerated code the device should be sent.

    Carries a code rather than leaving the router to classify: the router knows about sockets,
    not about why a model stopped talking, and a translation done in two places drifts.
    """

    def __init__(self, message: str, code: ErrorCode) -> None:
        super().__init__(message)
        self.code = code


@dataclass(slots=True)
class Orchestrator:
    """One turn at a time, streamed, per session.

    Implements the router's ``Responder`` seam. The router consumes an async iterator and knows
    nothing about Gemini, prompts or history -- which is what keeps the socket layer free of
    the model layer, and what let this class be written against the mock while the real adapter
    was built in parallel.
    """

    provider: LLMProvider
    system_prompt: str = field(default_factory=build_system_prompt)
    first_token_budget_s: float = DEFAULT_FIRST_TOKEN_BUDGET_S
    _history: dict[str, list[Message]] = field(default_factory=dict)

    def history(self, session_id: str) -> tuple[Message, ...]:
        """This session's conversation so far. A copy -- callers must not mutate it."""
        return tuple(self._history.get(session_id, ()))

    def forget(self, session_id: str) -> None:
        """Drop a session's history. Called when its connection ends."""
        self._history.pop(session_id, None)

    def respond(self, session_id: str, text: str) -> AsyncIterator[str]:
        """Run a turn and yield its deltas.

        Not ``async def``: the caller gets the iterator immediately and awaits deltas one at a
        time, which is what makes "the first delta leaves before the last is generated" a thing
        the router can actually do.
        """
        return self._run_turn(session_id, text)

    async def _run_turn(self, session_id: str, text: str) -> AsyncIterator[str]:
        history = self._history.setdefault(session_id, [])
        user_message = Message(role="user", text=text)
        history.append(user_message)

        stream = self.provider.stream(self.system_prompt, tuple(history))
        collected: list[str] = []

        try:
            first = await asyncio.wait_for(anext(stream), timeout=self.first_token_budget_s)
        except TimeoutError as exc:
            raise self._abort_turn(
                session_id,
                user_message,
                TurnAborted(
                    f"the model produced no token within {self.first_token_budget_s}s",
                    ErrorCode.LLM_TIMEOUT,
                ),
            ) from exc
        except StopAsyncIteration:
            # The model declined to say anything. Not a failure: ARCHITECTURE treats silence
            # as a clean end to a turn, and a spurious llm_failed here would put an error face
            # on the device for a turn that simply had no answer worth giving.
            #
            # The user message *stays*: the person did speak, and the next turn should know
            # it. This is the one place a turn ends without a reply and history is still right.
            log("turn.reply", chars=0, deltas=0, empty=True)
            return
        except ProviderError as exc:
            raise self._abort_turn(session_id, user_message, self._translate(exc)) from exc

        collected.append(first)
        yield first

        try:
            async for delta in stream:
                collected.append(delta)
                yield delta
        except ProviderError as exc:
            # Mid-stream death. The deltas already sent stay sent -- they were true when they
            # left -- but the turn must not be closed as if it had finished, and the partial
            # exchange must not enter history as though it were a complete one.
            raise self._abort_turn(session_id, user_message, self._translate(exc)) from exc

        history.append(Message(role="model", text="".join(collected)))
        log("turn.reply", chars=chars("".join(collected)), deltas=len(collected))

    def _abort_turn(
        self,
        session_id: str,
        user_message: Message,
        aborted: TurnAborted,
    ) -> TurnAborted:
        """Roll the failed turn out of history, then hand back the error to raise.

        ARCHITECTURE §Budgets and abort semantics: an aborted turn "rolls the user message
        back out of history so the next turn starts consistent". Without it the model would
        see, on the next turn, a question it never answered -- and would often answer *that*
        instead of what was just asked, which reads as the character ignoring you.

        Nothing of the partial reply is kept either: half an answer recorded as a whole one is
        a worse lie than no answer at all.
        """
        history = self._history.get(session_id)
        if history:
            # Remove *this* turn's message by identity, not by position: a concurrent turn on
            # the same session would make "the last one" the wrong one.
            for index in range(len(history) - 1, -1, -1):
                if history[index] is user_message:
                    del history[index]
                    break

        log("turn.aborted", code=str(aborted.code), level="warning")
        return aborted

    def _translate(self, exc: ProviderError) -> TurnAborted:
        """Translate a provider failure into the device's vocabulary.

        An unhinted failure becomes ``llm_failed`` **here, once**, rather than each adapter
        inventing a default -- a guessed ``rate_limited`` would send the device a face and a
        retry policy chosen for a reason that was never true.
        """
        return TurnAborted(str(exc), exc.code if exc.code is not None else ErrorCode.LLM_FAILED)

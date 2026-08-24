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
from contextlib import suppress
from dataclasses import dataclass, field
from typing import Final

from roboface_server.logging import chars, log
from roboface_server.prompt import build_system_prompt
from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import (
    LLMProvider,
    Message,
    ProviderError,
    TTSProvider,
)
from roboface_server.sentence import PhraseSplitter
from roboface_server.turn import AudioChunk, ReplyDelta, TurnEvent

#: The rolling conversation window, in messages. ARCHITECTURE §Data model specifies this
#: number -- *"``SessionMessage{...}`` -- the rolling 40-message window"* -- as part of the v4
#: data model. **The window is enforced here, from v0.2; what v4 adds is persistence**, not the
#: bound. Keeping them separate matters: without a bound, a long-lived connection costs more
#: per turn than the last one did, indefinitely, and eventually meets the model's context limit
#: as an ``llm_failed`` nobody can explain.
#:
#: 20 exchanges is a long conversation for a desk companion, and the oldest turns are the ones
#: a person has stopped meaning.
MAX_HISTORY_MESSAGES: Final = 40

#: How long the model has to produce its **first** delta. ARCHITECTURE §Budgets and abort
#: semantics puts the LLM's first-token budget at ~8 s. Nothing bounds the deltas after it --
#: a model mid-sentence is working, and cutting it off is worse than waiting.
DEFAULT_FIRST_TOKEN_BUDGET_S: Final = 8.0

#: How long TTS has to produce the **first chunk of a phrase**. ARCHITECTURE §Budgets and abort
#: semantics: the budget sits on the stage's first output, so a stalled vendor fails fast while a
#: slow-but-flowing one is never cut off mid-phrase. Shorter than the LLM's, because by the time a
#: phrase is ready to speak the person has already been waiting for the model to write it.
DEFAULT_FIRST_AUDIO_BUDGET_S: Final = 3.0


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
    #: Absent means a silent device, not a broken one: v0 had no speech and its tests still pass
    #: unchanged. Speech is added alongside the text, never instead of it.
    tts: TTSProvider | None = None
    first_audio_budget_s: float = DEFAULT_FIRST_AUDIO_BUDGET_S
    _history: dict[str, list[Message]] = field(default_factory=dict)

    def _remember(self, session_id: str, message: Message) -> None:
        """Append to a session's history, keeping it inside the window.

        Trimming from the front, so the messages that survive are the recent ones. A turn's own
        message is always the newest, so it can never be the one dropped to make room for it.
        """
        history = self._history.setdefault(session_id, [])
        history.append(message)
        if len(history) > MAX_HISTORY_MESSAGES:
            del history[: len(history) - MAX_HISTORY_MESSAGES]

    def history(self, session_id: str) -> tuple[Message, ...]:
        """This session's conversation so far. A copy -- callers must not mutate it."""
        return tuple(self._history.get(session_id, ()))

    def forget(self, session_id: str) -> None:
        """Drop a session's history. Called when its connection ends."""
        self._history.pop(session_id, None)

    def respond(self, session_id: str, text: str) -> AsyncIterator[TurnEvent]:
        """Run a turn and yield its deltas.

        Not ``async def``: the caller gets the iterator immediately and awaits deltas one at a
        time, which is what makes "the first delta leaves before the last is generated" a thing
        the router can actually do.
        """
        return self._run_turn(session_id, text)

    async def _run_turn(self, session_id: str, text: str) -> AsyncIterator[TurnEvent]:
        user_message = Message(role="user", text=text)
        self._remember(session_id, user_message)
        history = self._history[session_id]

        stream = self.provider.stream(self.system_prompt, tuple(history))
        collected: list[str] = []
        # A turn has exactly two endings: it completes, or it rolls back. Abandonment -- a
        # consumer that stops iterating -- was silently a third, leaving the user message
        # stranded with no reply and the provider's stream (an open HTTP response, in the real
        # adapter) waiting on garbage collection. Unreachable through today's router, which
        # tears the connection down; reachable in **v3.4**, where ARCHITECTURE §Budgets and
        # abort semantics has barge-in cancel a turn while the session continues. An invariant
        # with three outcomes is not an invariant, so it is closed here rather than inherited.
        #
        # Each ending below marks itself settled; the ``finally`` handles only the one nobody
        # wrote.
        settled = False

        try:
            try:
                first = await asyncio.wait_for(anext(stream), timeout=self.first_token_budget_s)
            except TimeoutError as exc:
                settled = True
                raise self._abort_turn(
                    session_id,
                    user_message,
                    TurnAborted(
                        f"the model produced no token within {self.first_token_budget_s}s",
                        ErrorCode.LLM_TIMEOUT,
                    ),
                ) from exc
            except StopAsyncIteration:
                # The model declined to say anything. Not a failure: ARCHITECTURE treats
                # silence as a clean end to a turn, and a spurious llm_failed here would put
                # an error face on the device for a turn that simply had no answer worth
                # giving.
                #
                # The user message *stays*: the person did speak, and the next turn should
                # know it. This is the one ending with no reply where history is still right.
                settled = True
                log("turn.reply", chars=0, deltas=0, empty=True)
                return
            except ProviderError as exc:
                settled = True
                raise self._abort_turn(session_id, user_message, self._translate(exc)) from exc

            collected.append(first)
            yield ReplyDelta(text=first)

            # The splitter decides when enough text exists to speak. Fed *after* the delta is
            # yielded, so the text never waits behind the audio: a phrase closing here is spoken
            # while the model is still writing the next one, which is the whole of v1.1.
            splitter = PhraseSplitter()
            try:
                for phrase in splitter.feed(first):
                    async for chunk in self._speak(phrase):
                        yield chunk

                async for delta in stream:
                    collected.append(delta)
                    yield ReplyDelta(text=delta)
                    for phrase in splitter.feed(delta):
                        async for chunk in self._speak(phrase):
                            yield chunk

                # The tail. Most replies from a desk companion are a sentence or two and end
                # without terminal punctuation ever arriving, so without this the last -- often
                # only -- phrase is never spoken.
                tail = splitter.flush()
                if tail:
                    async for chunk in self._speak(tail):
                        yield chunk
            except ProviderError as exc:
                # Mid-stream death, from either provider. The deltas and chunks already sent stay
                # sent -- they were true when they left -- but the turn must not be closed as if
                # it had finished, and the partial exchange must not enter history as though it
                # were a complete one.
                settled = True
                raise self._abort_turn(session_id, user_message, self._translate(exc)) from exc

            self._remember(session_id, Message(role="model", text="".join(collected)))
            settled = True
            log("turn.reply", chars=chars("".join(collected)), deltas=len(collected))
        finally:
            if not settled:
                self._abort_turn(
                    session_id,
                    user_message,
                    TurnAborted("the turn was abandoned before it finished", ErrorCode.INTERNAL),
                )
                await _close_quietly(stream)

    async def _speak(self, phrase: str) -> AsyncIterator[AudioChunk]:
        """Synthesize one phrase, yielding chunks as they arrive.

        Raises :class:`ProviderError` carrying ``tts_failed`` rather than aborting the turn itself:
        the caller already has one place that rolls a turn back, and a second would be a second
        chance to get the rollback wrong.
        """
        if self.tts is None:
            return

        stream = self.tts.synthesize(phrase)
        try:
            first = await asyncio.wait_for(anext(stream), timeout=self.first_audio_budget_s)
        except TimeoutError as exc:
            raise ProviderError(
                f"no audio within {self.first_audio_budget_s}s", ErrorCode.TTS_FAILED
            ) from exc
        except StopAsyncIteration:
            # Nothing to say for this phrase. Not a failure -- punctuation alone can close a
            # phrase that carries no speakable text.
            return

        yield AudioChunk(data=first)
        # No budget past the first chunk: a vendor mid-phrase is working, and cutting it off
        # would truncate a word rather than save any latency.
        async for chunk in stream:
            yield AudioChunk(data=chunk)

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


async def _close_quietly(stream: AsyncIterator[str]) -> None:
    """Close a provider stream, swallowing whatever it says on the way out.

    Cleanup runs on the failure path, which is exactly where a second exception would replace
    the first and lose the reason the turn ended. A provider that objects to being closed is
    not worth telling anyone about.
    """
    closer = getattr(stream, "aclose", None)
    if closer is None:
        return
    with suppress(Exception):
        await closer()

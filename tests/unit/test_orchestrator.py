"""Unit tests for the streaming turn.

Two properties carry this module and both are easy to lose silently: that deltas leave as they
arrive rather than at the end, and that the first-token budget distinguishes a provider which
has not started from one that is merely slow between tokens. A single timeout around the whole
stream would pass a naive test and truncate real replies.
"""

from __future__ import annotations

import asyncio

import pytest
from roboface_server.orchestrator import (
    DEFAULT_FIRST_TOKEN_BUDGET_S,
    Orchestrator,
    TurnAborted,
)
from roboface_server.protocol import ErrorCode
from roboface_server.providers import (
    Message,
    MockLLMProvider,
    ProviderError,
    ReplyText,
    SilentLLMProvider,
)
from roboface_server.turn import ReplyDelta


def _orchestrator(provider: object, **kwargs: object) -> Orchestrator:
    return Orchestrator(provider=provider, **kwargs)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------------------
# Streaming
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_turn_yields_the_provider_deltas_in_order() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["При", "віт", "!"]))

    deltas = [event.text async for event in orchestrator.respond("s", "hi")]

    assert deltas == ["При", "віт", "!"]


@pytest.mark.asyncio
async def test_the_first_delta_is_available_before_the_stream_ends() -> None:
    # The property, stated directly: a caller can act on delta 0 while the provider still has
    # work to do. An implementation that collected the reply would fail here and nowhere else.
    provider = MockLLMProvider(deltas=["a", "b", "c"], delay_s=0.2, delay_before_index=2)
    orchestrator = _orchestrator(provider)

    stream = orchestrator.respond("s", "hi")
    started = asyncio.get_running_loop().time()
    first = await anext(stream)
    elapsed = asyncio.get_running_loop().time() - started

    assert first == ReplyDelta(text="a")
    assert elapsed < 0.2, "the first delta waited for the rest of the stream"


@pytest.mark.asyncio
async def test_respond_is_not_a_coroutine_function() -> None:
    # The caller must get the iterator immediately, so it can await the *first* delta under
    # its own deadline rather than awaiting the whole turn.
    import inspect

    assert not inspect.iscoroutinefunction(_orchestrator(MockLLMProvider()).respond)


@pytest.mark.asyncio
async def test_the_provider_receives_the_system_prompt_and_the_history() -> None:
    provider = MockLLMProvider()
    orchestrator = _orchestrator(provider, system_prompt="SYSTEM")

    [event.text async for event in orchestrator.respond("s", "привіт")]

    system, messages = provider.calls[0]
    assert system == "SYSTEM"
    assert messages == (Message(role="user", text="привіт"),)


# ---------------------------------------------------------------------------------------
# The first-token budget -- both directions
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_budget_fires_when_the_provider_never_starts() -> None:
    provider = MockLLMProvider(delay_s=5.0, delay_before_index=0)
    orchestrator = _orchestrator(provider, first_token_budget_s=0.05)

    with pytest.raises(TurnAborted) as raised:
        [event.text async for event in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.LLM_TIMEOUT


@pytest.mark.asyncio
async def test_the_budget_does_not_fire_on_a_slow_middle() -> None:
    """The distinction the budget exists to make.

    A model mid-sentence is working. Cutting it off would truncate a healthy reply at exactly
    the moment it was going well -- which is what a single timeout around the whole stream
    would do, and why this test exists as the counterpart to the one above.
    """
    provider = MockLLMProvider(deltas=["a", "b", "c"], delay_s=0.15, delay_before_index=1)
    orchestrator = _orchestrator(provider, first_token_budget_s=0.05)

    deltas = [event.text async for event in orchestrator.respond("s", "hi")]

    assert deltas == ["a", "b", "c"]


def test_the_default_budget_matches_the_architecture() -> None:
    # ARCHITECTURE §Budgets and abort semantics puts the LLM first-token budget at ~8 s.
    assert DEFAULT_FIRST_TOKEN_BUDGET_S == 8.0


# ---------------------------------------------------------------------------------------
# History
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_history_accumulates_user_and_model_messages() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ві", "таю"]))

    [event.text async for event in orchestrator.respond("s", "привіт")]

    assert orchestrator.history("s") == (
        Message(role="user", text="привіт"),
        Message(role="model", text="вітаю"),
    )


@pytest.mark.asyncio
async def test_the_model_message_is_the_joined_deltas() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["one ", "two ", "three"]))

    [event.text async for event in orchestrator.respond("s", "count")]

    assert orchestrator.history("s")[-1].text == "one two three"


@pytest.mark.asyncio
async def test_a_second_turn_sees_the_first() -> None:
    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)

    [event.text async for event in orchestrator.respond("s", "first")]
    [event.text async for event in orchestrator.respond("s", "second")]

    _, messages = provider.calls[1]
    assert [message.text for message in messages] == ["first", "ok", "second"]


@pytest.mark.asyncio
async def test_two_sessions_do_not_share_a_history() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))

    [event.text async for event in orchestrator.respond("session-a", "from a")]
    [event.text async for event in orchestrator.respond("session-b", "from b")]

    assert [m.text for m in orchestrator.history("session-a")] == ["from a", "ok"]
    assert [m.text for m in orchestrator.history("session-b")] == ["from b", "ok"]


@pytest.mark.asyncio
async def test_forget_drops_a_session() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [event.text async for event in orchestrator.respond("s", "hi")]

    orchestrator.forget("s")

    assert orchestrator.history("s") == ()


@pytest.mark.asyncio
async def test_history_is_returned_as_a_copy() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [event.text async for event in orchestrator.respond("s", "hi")]

    snapshot = orchestrator.history("s")
    [event.text async for event in orchestrator.respond("s", "again")]

    assert len(snapshot) == 2, "a caller's snapshot changed underneath it"


# ---------------------------------------------------------------------------------------
# Failure translation (completed in RF-009)
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_an_unhinted_provider_failure_becomes_llm_failed() -> None:
    provider = MockLLMProvider(fail_at_index=0, error=ProviderError("no idea"))
    orchestrator = _orchestrator(provider)

    with pytest.raises(TurnAborted) as raised:
        [event.text async for event in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.LLM_FAILED


@pytest.mark.asyncio
async def test_a_hinted_provider_failure_keeps_its_code() -> None:
    error = ProviderError("quota", ErrorCode.RATE_LIMITED)
    orchestrator = _orchestrator(MockLLMProvider(fail_at_index=0, error=error))

    with pytest.raises(TurnAborted) as raised:
        [event.text async for event in orchestrator.respond("s", "hi")]

    assert raised.value.code is ErrorCode.RATE_LIMITED


@pytest.mark.asyncio
async def test_a_silent_provider_ends_the_turn_cleanly() -> None:
    # Not a failure: ARCHITECTURE treats silence as a clean end to a turn, and a spurious
    # llm_failed would put an error face on the device for a turn that had nothing to say.
    orchestrator = _orchestrator(SilentLLMProvider())

    deltas = [event.text async for event in orchestrator.respond("s", "hi")]

    assert deltas == []


# ---------------------------------------------------------------------------------------
# History rollback on abort (RF-009)
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_failure_before_the_first_delta_rolls_the_user_message_back() -> None:
    """Otherwise the next turn shows the model a question it never answered.

    ARCHITECTURE §Budgets and abort semantics requires this explicitly. The visible symptom of
    getting it wrong is the character answering the *previous* question instead of the one it
    was just asked.
    """
    orchestrator = _orchestrator(MockLLMProvider(fail_at_index=0))

    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "питання без відповіді")]

    assert orchestrator.history("s") == ()


@pytest.mark.asyncio
async def test_a_timeout_rolls_the_user_message_back() -> None:
    orchestrator = _orchestrator(
        MockLLMProvider(delay_s=5.0, delay_before_index=0), first_token_budget_s=0.05
    )

    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "hi")]

    assert orchestrator.history("s") == ()


@pytest.mark.asyncio
async def test_a_mid_stream_failure_rolls_back_and_keeps_no_partial_reply() -> None:
    # Half an answer recorded as a whole one is a worse lie than no answer at all.
    orchestrator = _orchestrator(MockLLMProvider(deltas=["почина", "ю"], fail_at_index=1))

    seen: list[str] = []
    with pytest.raises(TurnAborted):
        async for event in orchestrator.respond("s", "привіт"):
            seen.append(event.text)

    assert seen == ["почина"], "the deltas that did arrive should still have arrived"
    assert orchestrator.history("s") == ()


@pytest.mark.asyncio
async def test_the_next_turn_after_a_failure_sees_a_consistent_history() -> None:
    provider = MockLLMProvider(deltas=["добре"], fail_at_index=0)
    orchestrator = _orchestrator(provider)

    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "цей провалиться")]

    provider.fail_at_index = None
    [event.text async for event in orchestrator.respond("s", "цей спрацює")]

    _, messages = provider.calls[-1]
    assert [message.text for message in messages] == ["цей спрацює"], (
        "the failed turn's question was still in the history sent to the model"
    )
    assert [message.text for message in orchestrator.history("s")] == ["цей спрацює", "добре"]


@pytest.mark.asyncio
async def test_a_failure_does_not_disturb_earlier_successful_turns() -> None:
    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)
    [event.text async for event in orchestrator.respond("s", "перше")]

    provider.fail_at_index = 0
    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "друге")]

    assert [message.text for message in orchestrator.history("s")] == ["перше", "ok"]


@pytest.mark.asyncio
async def test_a_silent_reply_keeps_the_user_message() -> None:
    """The one case where a turn ends with no reply and history is still right.

    The person did speak. Rolling their message back would make the next turn behave as though
    they had not, which is a different bug from the one rollback exists to prevent.
    """
    orchestrator = _orchestrator(SilentLLMProvider())

    deltas = [event.text async for event in orchestrator.respond("s", "тиша")]

    assert deltas == []
    assert [message.text for message in orchestrator.history("s")] == ["тиша"]


@pytest.mark.asyncio
async def test_rollback_removes_this_turns_message_not_merely_the_last_one() -> None:
    """Identity, not position.

    Two turns interleaved on one session would make "the last message" the wrong one to
    remove, and the rollback would silently delete a healthy turn's question.
    """
    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)
    [event.text async for event in orchestrator.respond("s", "перше")]

    failing = MockLLMProvider(fail_at_index=0)
    orchestrator.provider = failing
    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "друге")]

    history = orchestrator.history("s")
    assert [message.text for message in history] == ["перше", "ok"]
    assert history[0].role == "user"
    assert history[1].role == "model"


# ---------------------------------------------------------------------------------------
# The invariant is total (code review #4)
# ---------------------------------------------------------------------------------------


class _ClosableProvider:
    """Records whether its stream was closed — what a real adapter's HTTP response needs."""

    def __init__(self, deltas: list[str]) -> None:
        self.deltas = deltas
        self.closed = False

    def stream(self, system: str, messages: object) -> object:
        return self._stream()

    async def _stream(self):  # type: ignore[no-untyped-def]
        try:
            for delta in self.deltas:
                yield ReplyText(text=delta)
        finally:
            self.closed = True


@pytest.mark.asyncio
async def test_an_abandoned_turn_rolls_back_rather_than_stranding_the_question() -> None:
    """The third ending a turn should not have.

    Unreachable through today's router, which tears the connection down. Reachable in v3.4,
    where barge-in cancels a turn while the session continues — at which point the next turn
    would show the model a question it never answered, which is exactly what the rollback in
    RF-009 exists to prevent.
    """
    orchestrator = _orchestrator(MockLLMProvider(deltas=["a", "b", "c", "d"]))

    stream = orchestrator.respond("s", "питання")
    seen = []
    async for event in stream:
        seen.append(event.text)
        if len(seen) == 2:
            break
    await stream.aclose()

    assert seen == ["a", "b"]
    assert orchestrator.history("s") == (), "the abandoned turn's question was left in history"


@pytest.mark.asyncio
async def test_an_abandoned_turn_closes_the_provider_stream() -> None:
    # In the real adapter that generator holds an open HTTP response. Leaving it to garbage
    # collection means leaking one per abandoned turn, on the path that abandons turns.
    provider = _ClosableProvider(["a", "b", "c"])
    orchestrator = _orchestrator(provider)

    stream = orchestrator.respond("s", "hi")
    async for _delta in stream:
        break
    await stream.aclose()

    assert provider.closed


@pytest.mark.asyncio
async def test_an_abandoned_turn_leaves_earlier_turns_intact() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [event.text async for event in orchestrator.respond("s", "перше")]

    stream = orchestrator.respond("s", "друге")
    async for _delta in stream:
        break
    await stream.aclose()

    assert [message.text for message in orchestrator.history("s")] == ["перше", "ok"]


@pytest.mark.asyncio
async def test_a_completed_turn_is_not_treated_as_abandoned() -> None:
    # The finally runs on every path; only the unhandled ending must trigger a rollback.
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ві", "таю"]))

    [event.text async for event in orchestrator.respond("s", "привіт")]

    assert [message.text for message in orchestrator.history("s")] == ["привіт", "вітаю"]


@pytest.mark.asyncio
async def test_a_silent_turn_is_not_treated_as_abandoned() -> None:
    # It ends without a reply *and* keeps its user message — the one ending where both are
    # correct. The abandonment branch must not undo it.
    orchestrator = _orchestrator(SilentLLMProvider())

    [event.text async for event in orchestrator.respond("s", "тиша")]

    assert [message.text for message in orchestrator.history("s")] == ["тиша"]


@pytest.mark.asyncio
async def test_every_ending_leaves_history_in_one_of_two_states() -> None:
    """The invariant itself, over every ending a turn has.

    Either the exchange is recorded whole, or the question is gone. Never a question with no
    answer, and never half an answer recorded as a whole one.
    """
    # 1. completes
    completing = _orchestrator(MockLLMProvider(deltas=["ok"]))
    [delta async for delta in completing.respond("s", "q")]
    assert [m.text for m in completing.history("s")] == ["q", "ok"]

    # 2. aborts before the first delta
    failing = _orchestrator(MockLLMProvider(fail_at_index=0))
    with pytest.raises(TurnAborted):
        [delta async for delta in failing.respond("s", "q")]
    assert failing.history("s") == ()

    # 3. aborts mid-stream
    midway = _orchestrator(MockLLMProvider(deltas=["a", "b"], fail_at_index=1))
    with pytest.raises(TurnAborted):
        [delta async for delta in midway.respond("s", "q")]
    assert midway.history("s") == ()

    # 4. times out
    stalled = _orchestrator(
        MockLLMProvider(delay_s=5.0, delay_before_index=0), first_token_budget_s=0.05
    )
    with pytest.raises(TurnAborted):
        [delta async for delta in stalled.respond("s", "q")]
    assert stalled.history("s") == ()

    # 5. is abandoned
    abandoned = _orchestrator(MockLLMProvider(deltas=["a", "b", "c"]))
    stream = abandoned.respond("s", "q")
    async for _delta in stream:
        break
    await stream.aclose()
    assert abandoned.history("s") == ()

    # 6. is silent — the sole ending with a question and no answer, and correct
    silent = _orchestrator(SilentLLMProvider())
    [delta async for delta in silent.respond("s", "q")]
    assert [m.text for m in silent.history("s")] == ["q"]


# ---------------------------------------------------------------------------------------
# The conversation window (hardening: v0.2 code review #5)
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_history_is_bounded_by_the_window() -> None:
    """Unbounded history costs more per turn than the last one did, forever.

    ARCHITECTURE §Data model specifies the number; what v4 adds is persistence, not the bound.
    """
    from roboface_server.orchestrator import MAX_HISTORY_MESSAGES

    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))

    for index in range(MAX_HISTORY_MESSAGES):  # two messages per turn
        [event.text async for event in orchestrator.respond("s", f"turn {index}")]

    assert len(orchestrator.history("s")) == MAX_HISTORY_MESSAGES


@pytest.mark.asyncio
async def test_the_window_keeps_the_recent_messages_not_the_first_ones() -> None:
    # A window that dropped the newest would be worse than none: the model would answer using
    # only what was said longest ago.
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))

    for index in range(30):
        [event.text async for event in orchestrator.respond("s", f"turn {index}")]

    texts = [message.text for message in orchestrator.history("s")]
    assert texts[-2:] == ["turn 29", "ok"]
    assert "turn 0" not in texts


@pytest.mark.asyncio
async def test_the_provider_never_receives_more_than_the_window() -> None:
    # The bound has to reach the *call*, not just the stored list -- the cost is in what is
    # sent, and a trim that happened after the send would save nothing.
    from roboface_server.orchestrator import MAX_HISTORY_MESSAGES

    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)

    for index in range(40):
        [event.text async for event in orchestrator.respond("s", f"turn {index}")]

    largest = max(len(messages) for _system, messages in provider.calls)
    assert largest <= MAX_HISTORY_MESSAGES


@pytest.mark.asyncio
async def test_a_turns_own_message_is_never_the_one_trimmed() -> None:
    # It is always the newest, so trimming from the front cannot reach it -- and if it could,
    # the model would be answering a question it had not been shown.
    from roboface_server.orchestrator import MAX_HISTORY_MESSAGES

    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)

    for index in range(MAX_HISTORY_MESSAGES + 5):
        [event.text async for event in orchestrator.respond("s", f"turn {index}")]

    _system, messages = provider.calls[-1]
    assert messages[-1].text == f"turn {MAX_HISTORY_MESSAGES + 4}"


@pytest.mark.asyncio
async def test_rollback_still_works_at_the_window_boundary() -> None:
    """Rollback finds its message by identity, and trimming may already have removed it.

    Both are "the question is not in history afterwards", which is the invariant that matters.
    """
    from roboface_server.orchestrator import MAX_HISTORY_MESSAGES

    provider = MockLLMProvider(deltas=["ok"])
    orchestrator = _orchestrator(provider)
    for index in range(MAX_HISTORY_MESSAGES):
        [event.text async for event in orchestrator.respond("s", f"turn {index}")]

    provider.fail_at_index = 0
    with pytest.raises(TurnAborted):
        [event.text async for event in orchestrator.respond("s", "this one fails")]

    texts = [message.text for message in orchestrator.history("s")]
    assert "this one fails" not in texts
    assert len(texts) <= MAX_HISTORY_MESSAGES


@pytest.mark.asyncio
async def test_the_window_is_per_session() -> None:
    orchestrator = _orchestrator(MockLLMProvider(deltas=["ok"]))

    for index in range(30):
        [event.text async for event in orchestrator.respond("busy", f"turn {index}")]
    [event.text async for event in orchestrator.respond("quiet", "one turn")]

    assert len(orchestrator.history("quiet")) == 2

"""Every way a turn can fail, and the code the device is sent for it.

One table, exhaustive over the failure modes v0.2 can produce. The point of enumerating them
in one place is that a new failure path with no row here is visible as an omission rather than
as a turn that quietly reports `internal`.
"""

from __future__ import annotations

import pytest
from roboface_server.orchestrator import Orchestrator, TurnAborted
from roboface_server.protocol import ErrorCode
from roboface_server.providers import MockLLMProvider, ProviderError


async def _run(orchestrator: Orchestrator) -> list[str]:
    return [delta async for delta in orchestrator.respond("s", "привіт")]


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("hint", "expected"),
    [
        (None, ErrorCode.LLM_FAILED),
        (ErrorCode.RATE_LIMITED, ErrorCode.RATE_LIMITED),
        (ErrorCode.LLM_FAILED, ErrorCode.LLM_FAILED),
        (ErrorCode.UNAUTHORIZED, ErrorCode.UNAUTHORIZED),
        (ErrorCode.SERVER_UNREACHABLE, ErrorCode.SERVER_UNREACHABLE),
    ],
    ids=["unhinted", "quota", "explicit-failure", "bad-key", "unreachable"],
)
async def test_a_provider_failure_maps_to_its_hint_or_to_llm_failed(
    hint: ErrorCode | None, expected: ErrorCode
) -> None:
    error = ProviderError("boom", hint)
    orchestrator = Orchestrator(provider=MockLLMProvider(fail_at_index=0, error=error))

    with pytest.raises(TurnAborted) as raised:
        await _run(orchestrator)

    assert raised.value.code is expected


@pytest.mark.asyncio
async def test_a_first_token_breach_maps_to_llm_timeout() -> None:
    orchestrator = Orchestrator(
        provider=MockLLMProvider(delay_s=5.0, delay_before_index=0),
        first_token_budget_s=0.05,
    )

    with pytest.raises(TurnAborted) as raised:
        await _run(orchestrator)

    assert raised.value.code is ErrorCode.LLM_TIMEOUT


@pytest.mark.asyncio
async def test_a_mid_stream_failure_maps_the_same_way_as_an_opening_one() -> None:
    # Where the failure happened changes what the device already received; it does not change
    # what went wrong, so it must not change the code.
    error = ProviderError("quota", ErrorCode.RATE_LIMITED)
    orchestrator = Orchestrator(
        provider=MockLLMProvider(deltas=["a", "b", "c"], fail_at_index=2, error=error)
    )

    with pytest.raises(TurnAborted) as raised:
        await _run(orchestrator)

    assert raised.value.code is ErrorCode.RATE_LIMITED


@pytest.mark.asyncio
async def test_every_mapped_code_is_a_real_member_of_the_enum() -> None:
    # Guards against a stringly-typed code creeping in: the device switches on this value.
    orchestrator = Orchestrator(provider=MockLLMProvider(fail_at_index=0))

    with pytest.raises(TurnAborted) as raised:
        await _run(orchestrator)

    assert raised.value.code in set(ErrorCode)


@pytest.mark.asyncio
async def test_a_turn_never_reports_internal_for_a_provider_problem() -> None:
    """`internal` means the server broke. A provider failing is not the server breaking.

    The same distinction `bad_frame` draws on the inbound side: a code that blames the wrong
    party sends the wrong face and points any retry logic at the wrong thing.
    """
    for hint in (None, ErrorCode.RATE_LIMITED, ErrorCode.LLM_FAILED):
        orchestrator = Orchestrator(
            provider=MockLLMProvider(fail_at_index=0, error=ProviderError("x", hint))
        )
        with pytest.raises(TurnAborted) as raised:
            await _run(orchestrator)

        assert raised.value.code is not ErrorCode.INTERNAL

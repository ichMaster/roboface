"""Contract test for the **`LLMProvider`** seam.

Written against the behaviour, not the annotations. `typing.get_type_hints` would happily agree
that `stream` returns an `AsyncIterator[str]` while the implementation collected the whole
reply and yielded it once — so every claim here is checked by *consuming* a stream and looking
at what actually came out.

This file changes only when the seam changes. Authority: ARCHITECTURE.md §Providers and seams,
§Model policy — Gemini only, and §Streaming is the architecture, not an optimisation.
"""

from __future__ import annotations

import inspect
from collections.abc import AsyncIterator

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers import (
    LLMProvider,
    Message,
    MockLLMProvider,
    ProviderError,
    SilentLLMProvider,
)

# ---------------------------------------------------------------------------------------
# The shape of the seam
# ---------------------------------------------------------------------------------------


def test_the_mock_satisfies_the_protocol() -> None:
    assert isinstance(MockLLMProvider(), LLMProvider)
    assert isinstance(SilentLLMProvider(), LLMProvider)


def test_stream_is_not_a_coroutine_function() -> None:
    """`stream` returns the iterator; it is not awaited first.

    This is what lets the orchestrator await the *first delta* under the first-token budget
    (ARCHITECTURE §Budgets and abort semantics) instead of awaiting the whole call. An
    `async def stream` would move that choice into the implementation, which is exactly where
    it must not live.
    """
    assert not inspect.iscoroutinefunction(MockLLMProvider().stream)


@pytest.mark.asyncio
async def test_stream_yields_deltas_one_at_a_time() -> None:
    provider = MockLLMProvider(deltas=["a", "b", "c"])

    stream = provider.stream("system", [Message(role="user", text="hi")])
    assert isinstance(stream, AsyncIterator)

    # Pulled individually, not collected: the assertion is that a delta is available before
    # the stream is exhausted, which is the whole property the seam exists to guarantee.
    first = await anext(stream)
    assert first == "a"

    rest = [delta async for delta in stream]
    assert rest == ["b", "c"]


@pytest.mark.asyncio
async def test_a_normal_reply_is_more_than_one_delta() -> None:
    # A single-delta default would let a buffering implementation pass every streaming test
    # in this repository.
    deltas = [delta async for delta in MockLLMProvider().stream("s", [])]

    assert len(deltas) > 1


def test_stream_takes_a_system_prompt_and_a_message_sequence() -> None:
    provider = MockLLMProvider()
    history = [Message(role="user", text="привіт"), Message(role="model", text="вітаю")]

    provider.stream("you are RoboFace", history)

    (system, messages) = provider.calls[0]
    assert system == "you are RoboFace"
    assert messages == tuple(history)


# ---------------------------------------------------------------------------------------
# Message
# ---------------------------------------------------------------------------------------


def test_message_is_a_frozen_role_and_text_pair() -> None:
    message = Message(role="user", text="hello")

    assert set(message.__dataclass_fields__) == {"role", "text"}
    with pytest.raises(AttributeError):
        message.text = "changed"  # type: ignore[misc]


def test_the_two_roles_are_user_and_model() -> None:
    # `model`, not `assistant`: the vendor-neutral word, and the one Gemini itself uses.
    assert Message(role="user", text="").role == "user"
    assert Message(role="model", text="").role == "model"


# ---------------------------------------------------------------------------------------
# ProviderError
# ---------------------------------------------------------------------------------------


def test_the_error_code_hint_is_optional() -> None:
    # An adapter that knows it was rate-limited says so; one that only knows the call failed
    # must not have to guess. RF-009 maps the missing hint to llm_failed in one place.
    assert ProviderError("something broke").code is None
    assert ProviderError("quota", ErrorCode.RATE_LIMITED).code is ErrorCode.RATE_LIMITED


def test_a_provider_error_is_an_exception() -> None:
    with pytest.raises(ProviderError):
        raise ProviderError("raised like anything else")


# ---------------------------------------------------------------------------------------
# Independence from any vendor
# ---------------------------------------------------------------------------------------


def test_importing_the_seam_pulls_in_no_vendor_sdk(repo_root: object) -> None:
    """The default suite must run with no API key, no network and no google-genai installed.

    A subprocess, for the same reason the protocol purity test uses one: by the time this
    runs, another test may already have imported anything.
    """
    import os
    import subprocess
    import sys

    program = (
        "import sys;"
        "import roboface_server.providers;"
        "banned = sorted(m for m in sys.modules "
        "if m.split('.')[0] in {'google', 'google_genai', 'httpx', 'requests'});"
        "print(','.join(banned))"
    )
    environment = dict(os.environ)
    environment["PYTHONPATH"] = "server"
    result = subprocess.run(
        [sys.executable, "-c", program],
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        env=environment,
        timeout=120,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "", f"the seam pulled in a vendor SDK: {result.stdout}"

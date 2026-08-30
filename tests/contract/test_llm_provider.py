"""Contract test for the **`LLMProvider`** seam.

Written against the behaviour, not the annotations. `typing.get_type_hints` would happily agree
that `stream` returns an `AsyncIterator[LLMEvent]` while the implementation collected the whole
reply and yielded it once — so every claim here is checked by *consuming* a stream and looking
at what actually came out.

**v2.2 changed this seam.** A reply is no longer only text: the model reports its own emotional
state alongside it, and the two arrive interleaved on one connection. The union is what makes the
report free — a second call asking "how do you feel about that?" would cost a round trip on every
turn, and by then the device has already spoken with the wrong face.

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
    ModelReport,
    ProviderError,
    ReplyText,
    SilentLLMProvider,
)


def _texts(events: list[object]) -> list[str]:
    return [event.text for event in events if isinstance(event, ReplyText)]

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
    # The report first -- see `test_the_report_precedes_the_text_it_describes`.
    assert isinstance(await anext(stream), ModelReport)

    first = await anext(stream)
    assert first == ReplyText(text="a")

    rest = [event async for event in stream]
    assert rest == [ReplyText(text="b"), ReplyText(text="c")]


@pytest.mark.asyncio
async def test_the_report_precedes_the_text_it_describes() -> None:
    """**Order is part of the contract.**

    The device should change expression as it begins to speak, not after it has finished. The only
    way to guarantee that is for the report to be the first thing the model produces -- which is
    why `gemini.py` declares `emotion` first in its response schema and pins the property ordering.
    A report that arrived last would be correct, useless, and impossible to notice.
    """
    events = [event async for event in MockLLMProvider(deltas=["a", "b"]).stream("s", [])]

    assert isinstance(events[0], ModelReport)
    assert _texts(events) == ["a", "b"]


@pytest.mark.asyncio
async def test_a_stream_may_carry_no_report() -> None:
    """Not a failure. The model answered, which is what a turn is for; the emotion engine falls
    back to `neutral`. A seam that required a report would fail turns over a face."""
    events = [event async for event in MockLLMProvider(report=None, deltas=["a"]).stream("s", [])]

    assert events == [ReplyText(text="a")]


def test_the_report_is_untyped_because_it_is_untrusted() -> None:
    """`ModelReport`'s fields are `object`, and that is the contract rather than laziness.

    Giving them their intended types here would be a claim that something has already checked
    them. Nothing has: this is raw model output, and `EmotionFrame.from_model` is where it becomes
    renderable. A seam that promised `Emotion` would be promising something no provider can keep.
    """
    report = ModelReport(emotion="not an emotion", intensity="not a number")
    assert report.emotion == "not an emotion"


@pytest.mark.asyncio
async def test_a_normal_reply_is_more_than_one_delta() -> None:
    # A single-delta default would let a buffering implementation pass every streaming test
    # in this repository.
    events = [event async for event in MockLLMProvider().stream("s", [])]

    assert len(_texts(events)) > 1


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

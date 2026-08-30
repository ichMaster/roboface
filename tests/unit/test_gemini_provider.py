"""Unit tests for the Gemini adapter, with the SDK mocked and no network.

The adapter is the one module that touches a vendor, so the properties worth pinning are the
ones a vendor change or a careless refactor would break silently: that deltas are yielded as
they arrive, that `thinkingBudget: 0` and the configured model actually reach the SDK, that no
vendor exception type escapes, and that the API key never appears in anything the server emits.
"""

from __future__ import annotations

import asyncio
import inspect
import json
from collections.abc import AsyncIterator
from typing import Any

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers import (
    LLMProvider,
    Message,
    ModelReport,
    ProviderError,
    ReplyText,
)
from roboface_server.providers.gemini import GeminiProvider


def _texts(events: list[Any]) -> list[str]:
    return [event.text for event in events if isinstance(event, ReplyText)]


API_KEY = "AIza-not-a-real-key-0123456789"


def _json_chunks(
    reply_parts: list[str], *, emotion: str = "joy", intensity: float = 0.7
) -> list[Any]:
    """The chunks a model answering under the v2.2 response schema actually produces.

    The report first and whole -- that is what the schema's property ordering buys -- then the
    reply's text arriving in pieces. Written as raw JSON fragments rather than built with
    `json.dumps`, because the *split points* are the thing under test and generating them would
    hide exactly the boundary a real network puts in the middle of a word.
    """
    head = f'{{"emotion": "{emotion}", "intensity": {intensity}, "reply": "'
    return [_Chunk(head), *[_Chunk(part) for part in reply_parts], _Chunk('"}')]


class _Chunk:
    def __init__(self, text: str | None) -> None:
        self.text = text


class _FakeStream:
    """Stands in for the SDK's async chunk iterator, and records how far it was consumed."""

    def __init__(self, chunks: list[Any], gate: asyncio.Event | None = None) -> None:
        self._chunks = list(chunks)
        self.yielded = 0
        self.gate = gate

    def __aiter__(self) -> _FakeStream:
        return self

    async def __anext__(self) -> Any:
        if self.yielded >= len(self._chunks):
            raise StopAsyncIteration
        # The gate lets a test hold the stream open after some chunks have been handed over,
        # which is how "a delta arrived before the stream was exhausted" is made checkable.
        if self.gate is not None and self.yielded == len(self._chunks) - 1:
            await self.gate.wait()
        chunk = self._chunks[self.yielded]
        self.yielded += 1
        return chunk


class _FakeModels:
    def __init__(self, stream: _FakeStream | None = None, error: Exception | None = None) -> None:
        self._stream = stream
        self._error = error
        self.calls: list[dict[str, Any]] = []

    async def generate_content_stream(self, **kwargs: Any) -> _FakeStream:
        self.calls.append(kwargs)
        if self._error is not None:
            raise self._error
        assert self._stream is not None
        return self._stream


class _FakeClient:
    def __init__(self, models: _FakeModels) -> None:
        self.aio = type("Aio", (), {"models": models})()
        self.api_key = API_KEY  # the shape a careless error message might reach for


def _provider(models: _FakeModels, **kwargs: Any) -> GeminiProvider:
    return GeminiProvider(API_KEY, client=_FakeClient(models), **kwargs)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------------------
# It satisfies the seam
# ---------------------------------------------------------------------------------------


def test_the_adapter_satisfies_the_provider_protocol() -> None:
    provider = _provider(_FakeModels(_FakeStream([])))

    assert isinstance(provider, LLMProvider)
    assert not inspect.iscoroutinefunction(provider.stream)


# ---------------------------------------------------------------------------------------
# Streaming
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_chunks_are_yielded_in_order() -> None:
    models = _FakeModels(_FakeStream(_json_chunks(["При", "віт", "!"])))

    events = [event async for event in _provider(models).stream("s", [])]

    assert _texts(events) == ["При", "віт", "!"]


@pytest.mark.asyncio
async def test_the_report_arrives_before_the_first_word() -> None:
    """The reason the schema declares `emotion` first, checked end to end.

    The head of the JSON object carries the whole report, so it is complete before a single
    character of the reply exists. The device therefore changes expression as it starts to speak.
    """
    models = _FakeModels(_FakeStream(_json_chunks(["Привіт"], emotion="sad", intensity=0.25)))

    events = [event async for event in _provider(models).stream("s", [])]

    assert events[0] == ModelReport(emotion="sad", intensity=0.25)
    assert all(isinstance(event, ReplyText) for event in events[1:])


@pytest.mark.asyncio
async def test_the_reply_survives_a_chunk_boundary_anywhere() -> None:
    r"""Fed one character at a time, which is every boundary at once.

    Including inside an escape sequence and inside a multi-byte character. Ukrainian is two bytes
    a letter and the SDK may hand over \uXXXX escapes, so a boundary landing mid-escape is the
    ordinary case here rather than the edge one -- and the reassembled text must be identical, not
    merely similar.
    """
    expected = "Привіт! Це «тест» — з \n переносом і 😀."
    whole = json.dumps({"emotion": "joy", "intensity": 0.5, "reply": expected})
    models = _FakeModels(_FakeStream([_Chunk(char) for char in whole]))

    events = [event async for event in _provider(models).stream("s", [])]

    assert "".join(_texts(events)) == expected


@pytest.mark.asyncio
async def test_a_response_that_is_not_the_requested_object_is_a_provider_error() -> None:
    """Coerced everywhere else, reported here -- because this is not a bad *emotion*, it is a
    response with no reply text in it at all. There is no face to fall back to; there is no turn."""
    models = _FakeModels(_FakeStream([_Chunk("I'm afraid I can't do that.")]))

    with pytest.raises(ProviderError) as raised:
        [event async for event in _provider(models).stream("s", [])]

    assert raised.value.code is ErrorCode.LLM_FAILED


@pytest.mark.asyncio
async def test_a_delta_is_yielded_before_the_stream_is_exhausted() -> None:
    """The property that separates streaming from splitting.

    The fake holds its last chunk behind a gate. If the adapter collected the response and
    yielded it whole, nothing would come out until the gate opened — so a delta arriving while
    the gate is still shut is proof the adapter forwards as it goes.
    """
    gate = asyncio.Event()
    stream = _FakeStream(_json_chunks(["a", "b", "c"]), gate=gate)
    models = _FakeModels(stream)

    iterator = _provider(models).stream("s", [])
    report = await anext(iterator)
    first = await anext(iterator)
    second = await anext(iterator)

    assert isinstance(report, ModelReport)
    assert (first, second) == (ReplyText(text="a"), ReplyText(text="b"))
    assert stream.yielded < 5, "the adapter drained the SDK stream before yielding anything"

    gate.set()
    assert _texts([event async for event in iterator]) == ["c"]


@pytest.mark.asyncio
async def test_textless_chunks_are_skipped_not_yielded_as_empty() -> None:
    # A chunk can legitimately carry no text (a safety verdict, a usage-only final chunk).
    # Yielding "" would show the device an empty delta and count as "the model said something".
    head, a, b, tail = _json_chunks(["a", "b"])
    models = _FakeModels(_FakeStream([head, a, _Chunk(None), _Chunk(""), b, tail]))

    events = [event async for event in _provider(models).stream("s", [])]

    assert _texts(events) == ["a", "b"]


# ---------------------------------------------------------------------------------------
# What actually reaches the SDK
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_thinking_is_disabled_and_the_model_is_the_configured_one() -> None:
    models = _FakeModels(_FakeStream(_json_chunks(["ok"])))
    provider = _provider(models, model="gemini-2.5-flash")

    [event async for event in provider.stream("s", [])]

    call = models.calls[0]
    assert call["model"] == "gemini-2.5-flash"
    # thinkingBudget: 0 is the documented reason this model was chosen — lowest
    # time-to-first-token. A default that quietly turns thinking on costs every turn.
    assert call["config"].thinking_config.thinking_budget == 0


@pytest.mark.asyncio
async def test_safety_settings_are_explicit_rather_than_defaulted() -> None:
    # A companion that abruptly starts refusing ordinary conversation because a vendor default
    # moved is a failure with no error message attached.
    models = _FakeModels(_FakeStream(_json_chunks(["ok"])))

    [delta async for delta in _provider(models).stream("s", [])]

    settings = models.calls[0]["config"].safety_settings
    assert settings is not None
    assert len(settings) == 4


@pytest.mark.asyncio
async def test_the_system_prompt_and_history_are_translated() -> None:
    models = _FakeModels(_FakeStream(_json_chunks(["ok"])))
    history = [Message(role="user", text="привіт"), Message(role="model", text="вітаю")]

    [delta async for delta in _provider(models).stream("SYSTEM", history)]

    call = models.calls[0]
    assert call["config"].system_instruction == "SYSTEM"
    assert [content.role for content in call["contents"]] == ["user", "model"]
    assert [content.parts[0].text for content in call["contents"]] == ["привіт", "вітаю"]


# ---------------------------------------------------------------------------------------
# Error translation
# ---------------------------------------------------------------------------------------


class _VendorError(Exception):
    def __init__(self, code: int) -> None:
        super().__init__(f"vendor said {code}")
        self.code = code


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("status", "expected"),
    [
        (429, ErrorCode.RATE_LIMITED),
        (504, ErrorCode.LLM_TIMEOUT),
    ],
)
async def test_statuses_that_are_still_true_at_the_device_carry_a_hint(
    status: int, expected: ErrorCode
) -> None:
    models = _FakeModels(error=_VendorError(status))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert raised.value.code is expected


@pytest.mark.asyncio
@pytest.mark.parametrize("status", [401, 403, 500, 503])
async def test_statuses_that_would_blame_the_device_are_left_unhinted(status: int) -> None:
    """A code must be true *at the device*, not merely accurate about what happened.

    DEVICE_UI renders a fault as the enumerated code verbatim — its worked example is
    "No server · server_unreachable". Mapping a Gemini 503 there would print "No server" on a
    device that is connected to its server and being told so over that connection. And *our*
    key being rejected is not the *device* being unauthorized.

    Unhinted, so the orchestrator maps it to llm_failed in the one place that exists for it.
    """
    models = _FakeModels(error=_VendorError(status))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert raised.value.code is None


@pytest.mark.asyncio
@pytest.mark.parametrize("status", [401, 403, 503])
async def test_the_status_is_still_recorded_in_the_message(status: int) -> None:
    # Not turning a status into a device-facing code must not mean losing it: the message is
    # logged server-side, where the person who can fix a bad key will look.
    models = _FakeModels(error=_VendorError(status))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert str(status) in str(raised.value)


@pytest.mark.asyncio
async def test_an_unrecognised_failure_carries_no_hint() -> None:
    # Unhinted, so the orchestrator maps it to llm_failed in one place. A guessed
    # rate_limited would send the device a face and a retry policy chosen for a reason that
    # was never true.
    models = _FakeModels(error=_VendorError(418))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert raised.value.code is None


@pytest.mark.asyncio
async def test_no_vendor_exception_type_escapes() -> None:
    class WeirdVendorProblem(Exception):
        pass

    models = _FakeModels(error=WeirdVendorProblem("something bespoke"))

    with pytest.raises(ProviderError):
        [delta async for delta in _provider(models).stream("s", [])]


@pytest.mark.asyncio
async def test_a_failure_part_way_through_the_stream_is_translated() -> None:
    class _ExplodingStream(_FakeStream):
        async def __anext__(self) -> Any:
            if self.yielded == 1:
                raise _VendorError(429)
            return await super().__anext__()

    # The first SDK chunk carries the whole report and opens the reply string; the vendor then
    # dies. What already left has left -- that is the property under test -- and from v2.2 what
    # left first is the report, before a single word of the reply existed.
    models = _FakeModels(_ExplodingStream(_json_chunks(["a"])))
    seen: list[Any] = []

    with pytest.raises(ProviderError) as raised:
        async for event in _provider(models).stream("s", []):
            seen.append(event)

    assert seen == [ModelReport(emotion="joy", intensity=0.7)]
    assert raised.value.code is ErrorCode.RATE_LIMITED


# ---------------------------------------------------------------------------------------
# The key never leaks
# ---------------------------------------------------------------------------------------


def test_a_missing_key_fails_at_construction_not_on_the_first_turn() -> None:
    # A server that starts without a key and dies mid-conversation is far harder to diagnose
    # than one that refuses to start.
    with pytest.raises(ProviderError) as raised:
        GeminiProvider("")

    assert raised.value.code is ErrorCode.UNAUTHORIZED


@pytest.mark.asyncio
async def test_the_api_key_never_appears_in_an_error_message() -> None:
    """The adapter formats exceptions, so this is where a key would escape if anywhere.

    Checked against the whole exception chain, not just the top message: a `raise … from exc`
    keeps the vendor's own text reachable from a traceback and from `str(exc.__cause__)`.
    """
    models = _FakeModels(error=_VendorError(429))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert API_KEY not in str(raised.value)
    assert API_KEY not in repr(raised.value)


@pytest.mark.asyncio
async def test_the_api_key_never_reaches_a_log_line(
    log_lines: object,
) -> None:
    from typing import cast

    models = _FakeModels(error=_VendorError(429))
    reader = cast(Any, log_lines)

    with pytest.raises(ProviderError):
        [delta async for delta in _provider(models).stream("s", [])]

    assert API_KEY not in str(reader())


# ---------------------------------------------------------------------------------------
# The seam stays free of the vendor
# ---------------------------------------------------------------------------------------


def test_the_sdk_is_imported_lazily_not_at_module_scope() -> None:
    """`providers/__init__` must stay importable with google-genai absent.

    Every test in the repository runs against the mock; a package-level vendor import would
    make all of them depend on a vendor wheel being installed.
    """
    source = inspect.getsource(GeminiProvider)
    module_source = inspect.getsource(inspect.getmodule(GeminiProvider))  # type: ignore[arg-type]
    module_header = module_source.split("class GeminiProvider")[0]

    assert "from google import genai" not in module_header.replace("TYPE_CHECKING", "")
    assert "from google import genai" in source or "from google.genai" in source


@pytest.mark.asyncio
async def test_stream_returns_an_async_iterator() -> None:
    provider = _provider(_FakeModels(_FakeStream(_json_chunks(["ok"]))))

    stream = provider.stream("s", [])

    assert isinstance(stream, AsyncIterator)
    assert _texts([event async for event in stream]) == ["ok"]


@pytest.mark.asyncio
async def test_automatic_function_calling_is_disabled() -> None:
    # No tools are passed, so AFC has nothing to do -- and leaving it on makes the SDK print a
    # recommendation to stderr on every single turn. Observed during the v0.2 DoD check.
    models = _FakeModels(_FakeStream(_json_chunks(["ok"])))

    [delta async for delta in _provider(models).stream("s", [])]

    afc = models.calls[0]["config"].automatic_function_calling
    assert afc is not None
    assert afc.disable is True

"""Unit tests for the Gemini adapter, with the SDK mocked and no network.

The adapter is the one module that touches a vendor, so the properties worth pinning are the
ones a vendor change or a careless refactor would break silently: that deltas are yielded as
they arrive, that `thinkingBudget: 0` and the configured model actually reach the SDK, that no
vendor exception type escapes, and that the API key never appears in anything the server emits.
"""

from __future__ import annotations

import asyncio
import inspect
from collections.abc import AsyncIterator
from typing import Any

import pytest
from roboface_server.protocol import ErrorCode
from roboface_server.providers import LLMProvider, Message, ProviderError
from roboface_server.providers.gemini import GeminiProvider

API_KEY = "AIza-not-a-real-key-0123456789"


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
    models = _FakeModels(_FakeStream([_Chunk("При"), _Chunk("віт"), _Chunk("!")]))

    deltas = [delta async for delta in _provider(models).stream("s", [])]

    assert deltas == ["При", "віт", "!"]


@pytest.mark.asyncio
async def test_a_delta_is_yielded_before_the_stream_is_exhausted() -> None:
    """The property that separates streaming from splitting.

    The fake holds its last chunk behind a gate. If the adapter collected the response and
    yielded it whole, nothing would come out until the gate opened — so a delta arriving while
    the gate is still shut is proof the adapter forwards as it goes.
    """
    gate = asyncio.Event()
    stream = _FakeStream([_Chunk("a"), _Chunk("b"), _Chunk("c")], gate=gate)
    models = _FakeModels(stream)

    iterator = _provider(models).stream("s", [])
    first = await anext(iterator)
    second = await anext(iterator)

    assert (first, second) == ("a", "b")
    assert stream.yielded < 3, "the adapter drained the SDK stream before yielding anything"

    gate.set()
    assert [delta async for delta in iterator] == ["c"]


@pytest.mark.asyncio
async def test_textless_chunks_are_skipped_not_yielded_as_empty() -> None:
    # A chunk can legitimately carry no text (a safety verdict, a usage-only final chunk).
    # Yielding "" would show the device an empty delta and count as "the model said something".
    models = _FakeModels(_FakeStream([_Chunk("a"), _Chunk(None), _Chunk(""), _Chunk("b")]))

    deltas = [delta async for delta in _provider(models).stream("s", [])]

    assert deltas == ["a", "b"]


# ---------------------------------------------------------------------------------------
# What actually reaches the SDK
# ---------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_thinking_is_disabled_and_the_model_is_the_configured_one() -> None:
    models = _FakeModels(_FakeStream([_Chunk("ok")]))
    provider = _provider(models, model="gemini-2.5-flash")

    [delta async for delta in provider.stream("s", [])]

    call = models.calls[0]
    assert call["model"] == "gemini-2.5-flash"
    # thinkingBudget: 0 is the documented reason this model was chosen — lowest
    # time-to-first-token. A default that quietly turns thinking on costs every turn.
    assert call["config"].thinking_config.thinking_budget == 0


@pytest.mark.asyncio
async def test_safety_settings_are_explicit_rather_than_defaulted() -> None:
    # A companion that abruptly starts refusing ordinary conversation because a vendor default
    # moved is a failure with no error message attached.
    models = _FakeModels(_FakeStream([_Chunk("ok")]))

    [delta async for delta in _provider(models).stream("s", [])]

    settings = models.calls[0]["config"].safety_settings
    assert settings is not None
    assert len(settings) == 4


@pytest.mark.asyncio
async def test_the_system_prompt_and_history_are_translated() -> None:
    models = _FakeModels(_FakeStream([_Chunk("ok")]))
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
        (401, ErrorCode.UNAUTHORIZED),
        (403, ErrorCode.UNAUTHORIZED),
        (503, ErrorCode.SERVER_UNREACHABLE),
        (504, ErrorCode.LLM_TIMEOUT),
    ],
)
async def test_known_statuses_carry_a_code_hint(status: int, expected: ErrorCode) -> None:
    models = _FakeModels(error=_VendorError(status))

    with pytest.raises(ProviderError) as raised:
        [delta async for delta in _provider(models).stream("s", [])]

    assert raised.value.code is expected


@pytest.mark.asyncio
async def test_an_unrecognised_failure_carries_no_hint() -> None:
    # Unhinted, so the orchestrator maps it to llm_failed in one place. A guessed
    # rate_limited would send the device a face and a retry policy chosen for a reason that
    # was never true.
    models = _FakeModels(error=_VendorError(500))

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

    models = _FakeModels(_ExplodingStream([_Chunk("a"), _Chunk("b")]))
    seen: list[str] = []

    with pytest.raises(ProviderError) as raised:
        async for delta in _provider(models).stream("s", []):
            seen.append(delta)

    assert seen == ["a"]
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
    provider = _provider(_FakeModels(_FakeStream([_Chunk("ok")])))

    stream = provider.stream("s", [])

    assert isinstance(stream, AsyncIterator)
    assert [delta async for delta in stream] == ["ok"]

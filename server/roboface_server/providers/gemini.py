"""The seam's one real implementation: Gemini 2.5 Flash.

MISSION §Principles and ARCHITECTURE §Model policy are unambiguous — *"Chat and vision run
on Gemini 2.5 Flash, with thinking disabled (``thinkingBudget: 0``), and on nothing else"*.
A second chat vendor is a non-goal, not a backlog item, so this module has no abstraction
inside it: the abstraction is :class:`~roboface_server.providers.base.LLMProvider`, and it
exists to keep the vendor out of the orchestrator and to keep the test suite free — not to
leave room for an alternative.

**The SDK is imported lazily, inside the constructor.** ``providers/__init__`` must stay
importable with ``google-genai`` absent: the whole default suite runs against the mock, and a
package-level import here would make every test in the repository depend on a vendor wheel.

**Thinking is off.** ``thinking_budget=0`` is the documented reason this model was chosen —
lowest time-to-first-token for a short conversational character. A reply that thinks first is a
reply the person waits through in silence.
"""

from __future__ import annotations

from collections.abc import AsyncIterator, Sequence
from typing import TYPE_CHECKING, Any, Final

from roboface_server.protocol import ErrorCode
from roboface_server.providers.base import Message, ProviderError

if TYPE_CHECKING:  # pragma: no cover -- typing only; never imported at runtime
    from google.genai import Client

#: Gemini's word for the assistant turn. ``Message.role`` uses ``model`` already, so the
#: mapping is the identity -- kept explicit so the day a vendor disagrees, the disagreement
#: has one place to live.
_ROLES: Final[dict[str, str]] = {"user": "user", "model": "model"}

#: HTTP statuses whose meaning survives the trip to the device. Everything else is left
#: **unhinted**, and the orchestrator maps it to ``llm_failed`` in the one place that exists
#: for it.
#:
#: The test is not "do we know what happened" but "is this code true *at the device*".
#: features/DEVICE_UI.md §Screens renders a fault as the enumerated code verbatim, with its
#: line -- its own worked example is "No server · ``server_unreachable``". So mapping a Gemini
#: 503 to ``server_unreachable`` would print "No server" on a device that is connected to its
#: server and being told so over that very connection: the one fault message a person could
#: act on, and it would be wrong. Likewise ``unauthorized`` for a 401: *our* key being bad is
#: not the *device* being unauthorized, and the device can do nothing with that.
#:
#: The real cause is not lost -- it is logged server-side, where the person who can fix a bad
#: key will actually look. This is the same "whose fault is it" split that produced
#: ``bad_frame`` in RF-009, applied on the outbound side where it was first got wrong.
_STATUS_HINTS: Final[dict[int, ErrorCode]] = {
    # Rate limiting is rate limiting wherever it happened, and the device's response to it
    # (back off, try again later) is the correct one.
    429: ErrorCode.RATE_LIMITED,
    # A gateway timeout on the LLM leg *is* an LLM timeout.
    504: ErrorCode.LLM_TIMEOUT,
}


class GeminiProvider:
    """Streams reply deltas from Gemini 2.5 Flash."""

    def __init__(
        self,
        api_key: str,
        *,
        model: str = "gemini-2.5-flash",
        thinking_budget: int = 0,
        client: Client | None = None,
    ) -> None:
        if not api_key and client is None:
            # Fail here rather than on the first turn: a server that starts without a key and
            # dies mid-conversation is far harder to diagnose than one that refuses to start.
            raise ProviderError("GEMINI_API_KEY is not set", ErrorCode.UNAUTHORIZED)

        self.model = model
        self.thinking_budget = thinking_budget
        self._client = client if client is not None else self._build_client(api_key)

    @staticmethod
    def _build_client(api_key: str) -> Client:
        try:
            from google import genai
        except ImportError as exc:  # pragma: no cover -- exercised by not installing the SDK
            raise ProviderError(
                "google-genai is not installed; it is required only for the real provider"
            ) from exc
        return genai.Client(api_key=api_key)

    def _config(self, system: str) -> Any:
        from google.genai import types

        return types.GenerateContentConfig(
            system_instruction=system,
            # Thinking off: the documented reason this model was chosen (lowest
            # time-to-first-token). See ARCHITECTURE §Model policy.
            thinking_config=types.ThinkingConfig(thinking_budget=self.thinking_budget),
            # No tools are passed, so automatic function calling has nothing to do -- and
            # leaving it on makes the SDK print a recommendation to stderr on *every* turn,
            # which would fill a production log with advice about a feature we do not use.
            automatic_function_calling=types.AutomaticFunctionCallingConfig(disable=True),
            # Set explicitly rather than left to a default that may change under us. A
            # desktop companion that abruptly starts refusing ordinary conversation because a
            # vendor default moved is a failure with no error message attached.
            safety_settings=[
                types.SafetySetting(
                    category=category,
                    threshold=types.HarmBlockThreshold.BLOCK_ONLY_HIGH,
                )
                for category in (
                    types.HarmCategory.HARM_CATEGORY_HARASSMENT,
                    types.HarmCategory.HARM_CATEGORY_HATE_SPEECH,
                    types.HarmCategory.HARM_CATEGORY_SEXUALLY_EXPLICIT,
                    types.HarmCategory.HARM_CATEGORY_DANGEROUS_CONTENT,
                )
            ],
        )

    @staticmethod
    def _contents(messages: Sequence[Message]) -> list[Any]:
        from google.genai import types

        return [
            types.Content(role=_ROLES[message.role], parts=[types.Part(text=message.text)])
            for message in messages
        ]

    def stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[str]:
        return self._stream(system, messages)

    async def _stream(self, system: str, messages: Sequence[Message]) -> AsyncIterator[str]:
        """Yield each chunk the moment it arrives.

        Never ``async for … : collected.append`` then yield the join. That would satisfy the
        type and destroy the property the type exists to express -- and it would not be
        visible in any test that only counts frames.
        """
        try:
            stream = await self._client.aio.models.generate_content_stream(
                model=self.model,
                contents=self._contents(messages),
                config=self._config(system),
            )
            async for chunk in stream:
                text = getattr(chunk, "text", None)
                # A chunk can legitimately carry no text -- a safety verdict, a usage-only
                # final chunk. Skipping it is right; yielding "" would show the device an
                # empty delta and, worse, count toward "the model said something".
                if text:
                    yield text
        except ProviderError:
            raise
        except Exception as exc:
            # Broad on purpose: stopping every vendor type here is the seam's whole job, and
            # a type that escaped would reach the orchestrator as an unhandled exception.
            raise self._translate(exc) from exc

    @staticmethod
    def _translate(exc: Exception) -> ProviderError:
        """Turn a vendor exception into the server's vocabulary.

        The orchestrator must never see a ``google.genai`` type; translating is the seam's
        entire job (ARCHITECTURE §Providers and seams). The message is deliberately built from
        the status and the exception class -- **never** from the client configuration -- so an
        API key cannot reach a log through an error string.
        """
        code = getattr(exc, "code", None)
        hint = _STATUS_HINTS.get(code) if isinstance(code, int) else None
        label = f"{type(exc).__name__} {code}" if code is not None else type(exc).__name__
        # The status stays in the message, which is logged server-side, even when it is
        # deliberately not turned into a device-facing code.
        return ProviderError(f"gemini request failed: {label}", hint)

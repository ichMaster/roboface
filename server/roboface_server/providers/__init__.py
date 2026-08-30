"""Provider seams and their implementations.

Importing this package pulls in the seam and the mocks and **nothing else** — in particular no
vendor SDK. `gemini.py` is imported by name, by the one place that selects it, so the default
test suite runs with no API key, no network and no `google-genai` installed.
"""

from roboface_server.emotion import ModelReport
from roboface_server.providers.base import (
    LLMEvent,
    LLMProvider,
    Message,
    ProviderError,
    ReplyText,
    Role,
)
from roboface_server.providers.mock import (
    DEFAULT_DELTAS,
    DEFAULT_REPORT,
    MockLLMProvider,
    SilentLLMProvider,
)

__all__ = [
    "DEFAULT_DELTAS",
    "DEFAULT_REPORT",
    "LLMEvent",
    "LLMProvider",
    "Message",
    "MockLLMProvider",
    "ModelReport",
    "ProviderError",
    "ReplyText",
    "Role",
    "SilentLLMProvider",
]

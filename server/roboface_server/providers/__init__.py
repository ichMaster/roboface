"""Provider seams and their implementations.

Importing this package pulls in the seam and the mocks and **nothing else** — in particular no
vendor SDK. `gemini.py` is imported by name, by the one place that selects it, so the default
test suite runs with no API key, no network and no `google-genai` installed.
"""

from roboface_server.providers.base import (
    LLMProvider,
    Message,
    ProviderError,
    Role,
)
from roboface_server.providers.mock import (
    DEFAULT_DELTAS,
    MockLLMProvider,
    SilentLLMProvider,
)

__all__ = [
    "DEFAULT_DELTAS",
    "LLMProvider",
    "Message",
    "MockLLMProvider",
    "ProviderError",
    "Role",
    "SilentLLMProvider",
]

"""Strip credential-shaped values before anything reaches disk.

Architecture §8. Hooks observe every Bash command, and a command line can carry a
model API key — so a naive tracker would write that key to disk on day one, breaking
the repo's standing rule that secrets live only in the agent's ``.env`` and are never
logged.

Redaction runs inside :func:`tracker.emit.emit`, unconditionally. Putting it at the
call sites would mean every call site can forget it, and one that does is silent.
"""

from __future__ import annotations

import re
from typing import Any

#: Replacement for a matched secret. Distinctive, so its presence is greppable.
MARKER = "«redacted»"

#: Patterns for values that are secrets by shape. Ordered longest-first where they
#: overlap, so a specific match wins over a general one.
PATTERNS: tuple[re.Pattern[str], ...] = (
    re.compile(r"\bsk-[A-Za-z0-9_\-]{16,}"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"\bxox[baprs]-[A-Za-z0-9\-]{10,}"),
    # KEY=value / "token": value — the assignment forms, which carry most leaks.
    re.compile(r"(?i)\b(api[_-]?key|secret|password|passwd|token)\b\s*[=:]\s*\S+"),
    re.compile(r"(?i)\bauthorization\s*:\s*\S+"),
)

#: Longest string kept verbatim; beyond this a value is truncated (architecture §4.3).
MAX_STRING = 512

#: How deep to walk before giving up. Redaction must not become slow enough to notice.
MAX_DEPTH = 12


def redact_text(text: str) -> str:
    """Replace every secret-shaped substring in one string."""
    for pattern in PATTERNS:
        text = pattern.sub(_replacement, text)
    return text


def _replacement(match: re.Match[str]) -> str:
    """Keep the key name when the match is an assignment, so the event stays readable.

    ``ANTHROPIC_API_KEY=sk-...`` becomes ``ANTHROPIC_API_KEY=«redacted»`` rather than
    a bare marker — which tells a reader *what* was withheld without leaking it.
    """
    whole = match.group(0)
    separator = "=" if "=" in whole else ":" if ":" in whole else ""
    if separator:
        head, _, _tail = whole.partition(separator)
        return f"{head}{separator}{MARKER}"
    return MARKER


def redact(value: Any, _depth: int = 0) -> Any:
    """Recursively redact strings inside dicts, lists and tuples.

    Non-string scalars pass through: a secret is never an int.
    """
    if _depth > MAX_DEPTH:
        return MARKER
    if isinstance(value, str):
        return redact_text(value)
    if isinstance(value, dict):
        return {key: redact(item, _depth + 1) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [redact(item, _depth + 1) for item in value]
    return value


def truncate(value: Any, limit: int = MAX_STRING, _depth: int = 0) -> tuple[Any, bool]:
    """Shorten over-long strings. Returns ``(value, was_truncated)``.

    Never drops the value: a truncated event is far better than a missing one.
    """
    truncated = False
    if _depth > MAX_DEPTH:
        return MARKER, True
    if isinstance(value, str):
        if len(value) > limit:
            return value[:limit] + "…", True
        return value, False
    if isinstance(value, dict):
        result_map: dict[Any, Any] = {}
        for key, item in value.items():
            result_map[key], hit = truncate(item, limit, _depth + 1)
            truncated = truncated or hit
        return result_map, truncated
    if isinstance(value, (list, tuple)):
        result_list: list[Any] = []
        for item in value:
            new_item, hit = truncate(item, limit, _depth + 1)
            result_list.append(new_item)
            truncated = truncated or hit
        return result_list, truncated
    return value, False

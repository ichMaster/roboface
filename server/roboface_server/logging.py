"""Structured logging, keyed by the connection it came from.

Every line is one JSON object carrying ``device_id`` and ``session_id``. That is not a
nicety: from v1 the server holds several devices at once, each running multi-stage turns, and
a log without those two keys is an unreadable interleaving of everyone's audio, everyone's
transcripts and everyone's errors.

The ids are bound **once per connection** into a :class:`~contextvars.ContextVar`, so every
coroutine underneath inherits them and no call site has to thread them by hand. asyncio
copies the context into each task, which is exactly the isolation two simultaneous devices
need -- and which the tests assert rather than assume.

**Two rules this module exists to enforce:**

* *The shape of a line never varies.* Before ``hello`` there is no ``device_id``; those lines
  carry an explicit ``null`` rather than omitting the key, so anything consuming the log can
  rely on the field being there.
* *Bodies and secrets never appear.* No ``.env`` value, no ``text_in``/``reply`` text -- a
  length instead. This is what keeps a log safe to paste into an issue, which is the only
  circumstance under which anyone actually reads one.
"""

from __future__ import annotations

import json
import logging
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass, replace
from datetime import UTC, datetime
from typing import Any, Final

#: The one logger the server emits through. Named so uvicorn's own configuration and this
#: one stay separable in a deployment.
LOGGER_NAME: Final = "roboface"

LEVELS: Final[dict[str, int]] = {
    "debug": logging.DEBUG,
    "info": logging.INFO,
    "warning": logging.WARNING,
    "error": logging.ERROR,
    "critical": logging.CRITICAL,
}

#: Where the per-call fields ride on a ``LogRecord``. Namespaced so it cannot collide with a
#: standard attribute -- ``extra`` overwrites silently, and a clash would be baffling.
FIELDS_ATTRIBUTE: Final = "roboface_fields"
CONTEXT_ATTRIBUTE: Final = "roboface_context"


@dataclass(frozen=True, slots=True)
class LogContext:
    """Who a line is about. ``device_id`` is ``None`` until ``hello`` is negotiated."""

    session_id: str | None = None
    device_id: str | None = None


#: Defaulted to ``None`` rather than to a ``LogContext()``: a ContextVar default is shared
#: by every context that never sets one, so the empty case is expressed as absence and
#: materialised per read instead.
_context: ContextVar[LogContext | None] = ContextVar("roboface_log_context", default=None)

#: What an unbound context looks like -- both ids null, never a missing key.
_UNBOUND: Final = LogContext()


@contextmanager
def connection_context(session_id: str) -> Iterator[None]:
    """Bind a connection's ids for the duration of the block.

    Reset on the way out so a pooled worker thread cannot carry one device's identity into
    the next connection it happens to serve.
    """
    token = _context.set(LogContext(session_id=session_id))
    try:
        yield
    finally:
        _context.reset(token)


def bind_device(device_id: str) -> None:
    """Attach the device id, once ``hello`` has told us what it is."""
    _context.set(replace(current_context(), device_id=device_id))


def current_context() -> LogContext:
    """The ids in force right now. Exposed for tests and for the formatter."""
    return _context.get() or _UNBOUND


class JsonFormatter(logging.Formatter):
    """One JSON object per line: ts, level, event, device_id, session_id, then the fields."""

    def format(self, record: logging.LogRecord) -> str:
        context: LogContext = getattr(record, CONTEXT_ATTRIBUTE, None) or current_context()
        payload: dict[str, Any] = {
            "ts": datetime.fromtimestamp(record.created, tz=UTC).isoformat(
                timespec="milliseconds"
            ),
            "level": record.levelname.lower(),
            "event": record.getMessage(),
            # Always present, even as null: a line whose shape depends on how far the
            # connection got is a line nothing can parse reliably.
            "device_id": context.device_id,
            "session_id": context.session_id,
        }
        payload.update(getattr(record, FIELDS_ATTRIBUTE, {}))
        return json.dumps(payload, ensure_ascii=False, default=str)


def configure(level: str = "info", *, stream: Any = None) -> logging.Logger:
    """Install the JSON handler at ``level`` (``ROBOFACE_LOG_LEVEL``). Idempotent."""
    logger = logging.getLogger(LOGGER_NAME)
    logger.setLevel(LEVELS[level])
    # Replace rather than append: calling this twice must not double every line.
    for existing in list(logger.handlers):
        logger.removeHandler(existing)
    handler = logging.StreamHandler(stream if stream is not None else sys.stderr)
    handler.setFormatter(JsonFormatter())
    logger.addHandler(handler)
    logger.propagate = False
    return logger


def log(event: str, *, level: str = "info", **fields: object) -> None:
    """Emit one structured line.

    ``event`` is a stable dotted name, not a sentence -- it is what a query groups by.
    Pass measurements, never bodies: ``chars=len(text)``, not ``text=text``.
    """
    logger = logging.getLogger(LOGGER_NAME)
    logger.log(
        LEVELS[level],
        event,
        # The context is captured *now*, not at format time: a handler may format later
        # (a queue, a background writer) by which point the contextvar has moved on.
        extra={FIELDS_ATTRIBUTE: dict(fields), CONTEXT_ATTRIBUTE: current_context()},
    )


def chars(text: str) -> int:
    """The measurement that goes in a log line where the text itself must not.

    A named helper rather than an inline ``len``, so the rule is greppable and a reviewer can
    see at the call site that a body was deliberately reduced to a number.
    """
    return len(text)

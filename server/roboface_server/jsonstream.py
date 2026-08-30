"""Reading a JSON object that is still arriving.

**Why this exists.** From v2.2 the model answers with an object rather than with text:
``{"emotion": …, "intensity": …, "reply": …}``. The obvious way to read it is to collect the whole
response and call ``json.loads``. That would cost the entire reply's generation time before the
device saw a single word — and time-to-first-delta is the number the whole architecture exists to
keep small. So the object is read *as it arrives*: the scalars are handed over the moment they
close, and the long string is handed over in pieces.

**What it is not.** Not a JSON parser. It reads the one shape this server asks for — a flat object
of scalars and strings — and it is not asked to survive nesting, arrays, or a malformed document
with anything more than "stop and say so". A general incremental parser is a much larger thing and
none of it would be exercised here.

**Pure**: no I/O, no clock, no provider. Fed ``str`` chunks, returns events. That is what makes the
chunk-boundary cases — inside a token, inside a string, inside an escape, inside a surrogate pair —
testable exhaustively rather than hopefully.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from enum import Enum, auto


@dataclass(frozen=True, slots=True)
class Field:
    """A complete value, the moment it closed."""

    name: str
    value: object


@dataclass(frozen=True, slots=True)
class TextDelta:
    """Part of a streamed string's contents.

    Concatenating every delta for one name reproduces that string exactly, including whatever the
    escapes decoded to. A test asserts precisely that, because "streams the text" and "streams the
    text correctly" are different claims and only the second one is useful.
    """

    name: str
    text: str


JsonEvent = Field | TextDelta


class _State(Enum):
    BEFORE_OBJECT = auto()
    SEEK_KEY = auto()
    KEY = auto()
    SEEK_COLON = auto()
    SEEK_VALUE = auto()
    STRING = auto()
    SCALAR = auto()
    DONE = auto()


#: What a backslash escape means. ``u`` is absent because it is not a substitution but a length.
_ESCAPES = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}

_WHITESPACE = " \t\r\n"


class MalformedJson(ValueError):
    """The document is not the shape this reader was promised.

    Raised rather than swallowed: unlike a model's *emotion*, which is coerced because a face is
    not worth a failed turn, a response that is not JSON at all means the reply text itself is
    unavailable. The caller has a real decision to make about that, so it must be told.
    """


class ObjectStream:
    """Feed it chunks; it yields events.

    ``stream_keys`` names the string fields whose contents come out as :class:`TextDelta` while
    they arrive. Everything else is buffered until it closes and comes out as a :class:`Field`.
    A streamed key produces no ``Field`` — its value was already delivered, and emitting it twice
    would make "did the reply arrive" ambiguous.
    """

    def __init__(self, stream_keys: frozenset[str] = frozenset()) -> None:
        self._stream_keys = stream_keys
        self._state = _State.BEFORE_OBJECT
        self._key = ""
        self._buffer: list[str] = []
        #: Set while the previous chunk ended inside an escape: ``""`` after a lone backslash,
        #: or ``"uXX"`` partway through a ``\uXXXX``. The whole reason this class exists rather
        #: than a regex -- a boundary can fall anywhere, including here.
        self._escape: str | None = None
        #: A high surrogate awaiting its pair. JSON spells astral characters as two ``\uXXXX``
        #: escapes, and emitting the first alone produces a lone surrogate that cannot be encoded.
        self._surrogate: str | None = None
        self._streaming = False
        #: Streamed text accumulated within the current chunk, flushed as **one** delta.
        #:
        #: Per-character deltas would be correct and unusable: each one becomes a `reply` frame on
        #: the wire, so a 400-character answer would be 400 websocket frames instead of the dozen
        #: the network actually delivered. One delta per chunk keeps the granularity the model and
        #: the network already chose, which is the right one -- nothing is delayed by it, because
        #: the chunk is the unit in which the text arrived in the first place.
        self._pending: list[str] = []

    def feed(self, chunk: str) -> list[JsonEvent]:
        """Consume one chunk. Returns everything that became known because of it."""
        events: list[JsonEvent] = []
        for char in chunk:
            self._step(char, events)
        self._flush(events)
        return events

    def _flush(self, events: list[JsonEvent]) -> None:
        if self._pending:
            events.append(TextDelta(self._key, "".join(self._pending)))
            self._pending = []

    def finish(self) -> list[JsonEvent]:
        """End of input. Raises if the document stopped mid-value.

        A truncated response is a real failure and a silent one otherwise: the caller would see a
        short reply and no error, which is indistinguishable from the model being brief.
        """
        if self._state in (_State.DONE, _State.BEFORE_OBJECT):
            return []
        if self._state is _State.SCALAR:
            # A number at the very end of input, with the closing brace missing. The value is
            # complete even though the document is not, so it is worth handing over.
            events: list[JsonEvent] = []
            self._close_scalar(events)
            self._state = _State.DONE
            return events
        raise MalformedJson(f"input ended inside a value (state {self._state.name})")

    # ----------------------------------------------------------------------------------

    def _step(self, char: str, events: list[JsonEvent]) -> None:
        match self._state:
            case _State.BEFORE_OBJECT:
                if char == "{":
                    self._state = _State.SEEK_KEY
                elif char not in _WHITESPACE:
                    raise MalformedJson(f"expected an object, got {char!r}")

            case _State.SEEK_KEY:
                if char == '"':
                    self._buffer = []
                    self._state = _State.KEY
                elif char == "}":
                    self._state = _State.DONE
                elif char not in _WHITESPACE and char != ",":
                    raise MalformedJson(f"expected a key, got {char!r}")

            case _State.KEY:
                # Keys are the schema's own names -- no escapes to decode, so this stays a scan.
                if char == '"':
                    self._key = "".join(self._buffer)
                    self._buffer = []
                    self._state = _State.SEEK_COLON
                else:
                    self._buffer.append(char)

            case _State.SEEK_COLON:
                if char == ":":
                    self._state = _State.SEEK_VALUE
                elif char not in _WHITESPACE:
                    raise MalformedJson(f"expected ':' after {self._key!r}, got {char!r}")

            case _State.SEEK_VALUE:
                if char in _WHITESPACE:
                    return
                self._buffer = []
                if char == '"':
                    self._streaming = self._key in self._stream_keys
                    self._state = _State.STRING
                else:
                    self._buffer.append(char)
                    self._state = _State.SCALAR

            case _State.STRING:
                self._string_char(char, events)

            case _State.SCALAR:
                if char in ",}" or char in _WHITESPACE:
                    self._close_scalar(events)
                    self._state = _State.DONE if char == "}" else _State.SEEK_KEY
                else:
                    self._buffer.append(char)

            case _State.DONE:
                if char not in _WHITESPACE:
                    raise MalformedJson(f"trailing content after the object: {char!r}")

    def _string_char(self, char: str, events: list[JsonEvent]) -> None:
        if self._escape is not None:
            self._escape_char(char, events)
            return

        if char == "\\":
            self._escape = ""
            return

        if char == '"':
            if self._surrogate is not None:
                raise MalformedJson("string ended on an unpaired surrogate")
            if self._streaming:
                # Flush before the key changes: `_pending` is labelled by `self._key`, and the very
                # next character may begin a different field.
                self._flush(events)
            else:
                events.append(Field(self._key, "".join(self._buffer)))
            self._buffer = []
            self._streaming = False
            self._state = _State.SEEK_KEY
            return

        self._emit_text(char, events)

    def _escape_char(self, char: str, events: list[JsonEvent]) -> None:
        assert self._escape is not None
        pending = self._escape

        if pending == "":
            if char == "u":
                self._escape = "u"
                return
            decoded = _ESCAPES.get(char)
            if decoded is None:
                raise MalformedJson(f"unknown escape \\{char}")
            self._escape = None
            self._emit_text(decoded, events)
            return

        # Partway through \uXXXX. Accumulate until four hex digits are in hand -- which is where a
        # chunk boundary is most likely to land in Ukrainian text, since every letter is one.
        pending += char
        if len(pending) < 5:
            self._escape = pending
            return

        self._escape = None
        try:
            code = int(pending[1:], 16)
        except ValueError as exc:
            raise MalformedJson(f"bad unicode escape \\{pending}") from exc

        if 0xD800 <= code <= 0xDBFF:
            if self._surrogate is not None:
                raise MalformedJson("two high surrogates in a row")
            self._surrogate = chr(code)
            return

        if 0xDC00 <= code <= 0xDFFF:
            if self._surrogate is None:
                raise MalformedJson("low surrogate with no high surrogate")
            pair = (self._surrogate + chr(code)).encode("utf-16", "surrogatepass").decode("utf-16")
            self._surrogate = None
            self._emit_text(pair, events)
            return

        if self._surrogate is not None:
            raise MalformedJson("high surrogate followed by an ordinary character")
        self._emit_text(chr(code), events)

    def _emit_text(self, text: str, events: list[JsonEvent]) -> None:
        del events  # streamed text is batched in `_pending` and flushed by `_flush`
        if self._streaming:
            self._pending.append(text)
        else:
            self._buffer.append(text)

    def _close_scalar(self, events: list[JsonEvent]) -> None:
        token = "".join(self._buffer).strip()
        self._buffer = []
        try:
            events.append(Field(self._key, json.loads(token)))
        except ValueError as exc:
            raise MalformedJson(f"{self._key!r} is not a JSON value: {token!r}") from exc

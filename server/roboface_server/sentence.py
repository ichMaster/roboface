"""Deciding when a phrase is finished enough to speak.

ARCHITECTURE §Audio pipeline: the orchestrator hands *completed clauses* to TTS while the model is
still generating the rest. This module is the "completed" judgement, and it is pure — no provider,
no protocol, no I/O — because every failure it can have is audible and none of them need a network
to reproduce.

**The rule is a boundary followed by whitespace, and the whitespace is the whole point.** A period
alone is not the end of a sentence: `3.5` contains one, and so does every Ukrainian abbreviation.
Requiring the *next* character to be whitespace turns "is this a sentence end?" from a guess into an
observation — and it also solves the streaming problem, because a boundary sitting at the end of the
buffer has no next character yet. Waiting for one costs a few milliseconds; not waiting means saying
"three point" and then "five" as two phrases, in two separate synthesis calls, with a pause between
them.
"""

from __future__ import annotations

from typing import Final

#: Characters that can end a speakable phrase. `;` is here because a long clause is worth speaking
#: before the sentence it belongs to finishes -- the whole phase exists to start the mouth early.
BOUNDARIES: Final = frozenset(".!?…;")

#: Ukrainian abbreviations that end in a period which is **not** a sentence end. Without these the
#: splitter cuts "і т.д." into "і т." and "д.", and the listener hears two fragments.
#:
#: Matched case-insensitively and only at a word boundary, so `ст.` does not fire inside `віст.`
ABBREVIATIONS: Final = (
    "напр.",
    "т.д.",
    "т.п.",
    "т.зв.",
    "вул.",
    "просп.",
    "обл.",
    "р-н.",
    "ім.",
    "ін.",
    "рр.",
    "ст.",
    "стор.",
    "див.",
    "гр.",
    "млн.",
    "млрд.",
)

#: How long a clause may get before it is spoken anyway. A model that writes without punctuation
#: would otherwise buffer to the end of the turn and speak in one burst -- which is exactly the
#: request/response behaviour this phase removes.
DEFAULT_MAX_CHARS: Final = 160


class PhraseSplitter:
    """Turns a stream of LLM deltas into speakable phrases.

    Stateful across `feed` calls by design: a delta is whatever the model emitted, often a partial
    word and sometimes a single character, so no individual delta can be judged on its own.
    """

    def __init__(self, max_chars: int = DEFAULT_MAX_CHARS) -> None:
        self._buffer = ""
        self._max_chars = max_chars

    def feed(self, delta: str) -> list[str]:
        """Absorb one delta and return whatever phrases it completed, in order."""
        self._buffer += delta
        phrases: list[str] = []

        while True:
            index = self._boundary_index()
            if index is None:
                break
            phrase = self._buffer[: index + 1].strip()
            self._buffer = self._buffer[index + 1 :].lstrip()
            if phrase:
                phrases.append(phrase)

        self._force_flush(phrases)
        return phrases

    def flush(self) -> str | None:
        """The tail, once the reply is over. ``None`` when there is nothing left to say.

        Without this a short answer with no terminal punctuation -- which is most of them, for a
        companion that replies in a sentence or two -- would never be spoken at all.
        """
        tail = self._buffer.strip()
        self._buffer = ""
        return tail or None

    @property
    def pending(self) -> str:
        """What is still buffered. For tests and diagnostics; not part of the streaming path."""
        return self._buffer

    def _boundary_index(self) -> int | None:
        """The index of the first boundary that genuinely ends a phrase, or ``None``."""
        for index, char in enumerate(self._buffer):
            if char not in BOUNDARIES:
                continue
            if index + 1 >= len(self._buffer):
                # The boundary is the last character we have. It may be a decimal point whose digit
                # has not arrived yet, so this is not "no boundary" -- it is "not yet", and the
                # difference is a half-spoken number.
                return None
            if not self._buffer[index + 1].isspace():
                continue
            if self._ends_with_abbreviation(index):
                continue
            return index
        return None

    def _ends_with_abbreviation(self, index: int) -> bool:
        """Whether the text ending at ``index`` is one of the known abbreviations."""
        text = self._buffer[: index + 1].lower()
        for abbreviation in ABBREVIATIONS:
            if not text.endswith(abbreviation):
                continue
            before = text[: len(text) - len(abbreviation)]
            # A word boundary before it, so `ст.` does not fire inside `віст.`
            if not before or not before[-1].isalpha():
                return True
        return False

    def _force_flush(self, phrases: list[str]) -> None:
        """Speak a runaway clause rather than buffering it to the end of the turn.

        Cuts at the last space that fits. If there is no space at all the buffer is one unbroken
        word, and it is left alone: cutting there would split a word down the middle, which is the
        one thing the acceptance criteria forbid, and a word ends soon enough on its own.
        """
        while len(self._buffer) > self._max_chars:
            cut = self._buffer.rfind(" ", 0, self._max_chars + 1)
            if cut <= 0:
                return
            phrase = self._buffer[:cut].strip()
            self._buffer = self._buffer[cut:].lstrip()
            if phrase:
                phrases.append(phrase)

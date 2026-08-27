"""Deciding when a person has finished a thought.

Pure — no provider, no protocol, no clock of its own. It consumes `ASRChunk`s and an `UtteranceEnd`
signal and answers one question: *is there something to reply to yet?*

**The phrase-hold is the whole point.** Deepgram's `speech_final` means "this span is settled", not
"the speaker has stopped". A person taking a breath mid-sentence produces one, and it usually
arrives **without terminal punctuation** — `smart_format` punctuates what it believes is a finished
sentence. So an un-punctuated final is held: for the continuation that follows, or for the
`UtteranceEnd` backstop if none does.

Answering it immediately is not a subtle latency bug. It is the character talking over someone
mid-sentence, which reads as rudeness rather than as a defect, and no amount of speed makes up for
it. Waiting costs the backstop's ~1 s, and only in the case where the person genuinely stopped on an
unpunctuated word.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Final

from roboface_server.providers.base import ASRChunk

#: Characters that end a thought. If `smart_format` put one of these at the end, the vendor believes
#: the sentence is complete and there is nothing to wait for.
TERMINATORS: Final = ".!?…"


@dataclass(slots=True)
class UtteranceTracker:
    """Assembles one utterance from a stream of recognition events."""

    #: Finals that are settled *and* complete, in order.
    _settled: list[str] = field(default_factory=list)
    #: A final with no terminal punctuation, waiting for a continuation or the backstop.
    _held: str = ""
    #: The newest interim, which replaces rather than accumulates.
    _interim: str = ""

    def feed(self, chunk: ASRChunk) -> str | None:
        """Absorb one chunk. Returns the utterance if it is now complete, else ``None``."""
        text = chunk.text.strip()

        if not chunk.is_final:
            # Interims *replace*. Deepgram revises them constantly -- "прив" becomes "привіт"
            # becomes "привіт як" -- so accumulating produces every draft concatenated.
            self._interim = text
            return None

        self._interim = ""
        if not text:
            return None

        if self._held:
            # The continuation of a held phrase. Joined with a space: the two halves were separate
            # recognition spans and neither carries the whitespace between them.
            text = f"{self._held} {text}"
            self._held = ""

        if text[-1] in TERMINATORS:
            self._settled.append(text)
            return self._resolve()

        # Settled but unfinished: a breath, not an ending.
        self._held = text
        return None

    def utterance_end(self) -> str | None:
        """The vendor's backstop fired: no more speech is coming.

        Releases whatever is held, so a held phrase can never wait forever -- which is the failure
        the hold would otherwise introduce, and a worse one than answering early.
        """
        if self._held:
            self._settled.append(self._held)
            self._held = ""
        return self._resolve()

    def finish(self) -> str | None:
        """The audio window closed. Same as the backstop, for a device that stopped sending."""
        return self.utterance_end()

    @property
    def partial(self) -> str:
        """What to show as ``asr_partial`` right now: everything settled, plus the live interim."""
        parts = [*self._settled]
        if self._held:
            parts.append(self._held)
        if self._interim:
            parts.append(self._interim)
        return " ".join(parts).strip()

    @property
    def pending(self) -> bool:
        """Whether anything is being held back. For diagnostics, not for the decision."""
        return bool(self._held)

    def reset(self) -> None:
        self._settled.clear()
        self._held = ""
        self._interim = ""

    def _resolve(self) -> str | None:
        """The finished utterance, or ``None`` when nothing was said.

        Empty is not an error: v1.2's press-and-hold produces a zero-byte utterance on a very short
        hold, and the correct answer to silence is silence.
        """
        text = " ".join(self._settled).strip()
        self.reset()
        return text or None

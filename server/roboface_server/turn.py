"""What a turn emits, as it emits it.

v0.2's `Responder` yielded `str` deltas, because text was the only thing a turn produced. v1.1 adds
a second stream — synthesized audio — and the two are **interleaved**: a phrase's audio leaves while
later words are still being generated. That is the whole point of the phase, and it is why this is a
union of events rather than two separate iterators. Two iterators would have to be merged by
whoever consumed them, and the merge is exactly where the interleaving would be lost.

These are not wire frames. `ReplyDelta` becomes a `reply` text frame and `AudioChunk` becomes an
unlabelled binary one, but that translation belongs to the router: the orchestrator should not know
how a chunk is framed, only that it exists and that it is ready now.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class ReplyDelta:
    """One piece of the model's text, ready to send."""

    text: str


@dataclass(frozen=True, slots=True)
class AudioChunk:
    """PCM16 16 kHz mono, ready to play.

    Carries no phrase or sequence number. The device plays what arrives in the order it arrives,
    and the speaking window is delimited by the connection's phase rather than by anything in the
    payload -- adding an index here would create a second, redundant source of truth about order.
    """

    data: bytes


#: Everything a turn can emit.
TurnEvent = ReplyDelta | AudioChunk

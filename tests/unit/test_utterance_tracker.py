"""The utterance tracker, against Deepgram-shaped sequences.

Every test here is a sequence rather than a value, because the rule being checked is about *when* a
thought is finished — and that is only visible across several events.
"""

from __future__ import annotations

from roboface_server.providers.base import ASRChunk
from roboface_server.utterance import UtteranceTracker


def interim(text: str) -> ASRChunk:
    return ASRChunk(text=text, is_final=False)


def final(text: str) -> ASRChunk:
    return ASRChunk(text=text, is_final=True)


def test_a_punctuated_final_resolves_at_once() -> None:
    tracker = UtteranceTracker()
    assert tracker.feed(interim("привіт")) is None
    assert tracker.feed(final("Привіт, як справи?")) == "Привіт, як справи?"


def test_interims_replace_rather_than_accumulate() -> None:
    # Deepgram revises: "прив" becomes "привіт" becomes "привіт як". Accumulating produces every
    # draft concatenated, which is what appears on screen if this is wrong.
    tracker = UtteranceTracker()
    tracker.feed(interim("прив"))
    tracker.feed(interim("привіт"))
    tracker.feed(interim("привіт як"))
    assert tracker.partial == "привіт як"


def test_an_unpunctuated_final_is_held_not_answered() -> None:
    # A breath mid-sentence. Answering here is the character talking over someone.
    tracker = UtteranceTracker()
    assert tracker.feed(final("Я хотів запитати")) is None
    assert tracker.pending


def test_a_held_phrase_is_joined_with_its_continuation() -> None:
    tracker = UtteranceTracker()
    tracker.feed(final("Я хотів запитати"))
    assert tracker.feed(final("чи ти мене чуєш?")) == "Я хотів запитати чи ти мене чуєш?"


def test_a_held_phrase_is_released_by_the_backstop() -> None:
    # The hold's own failure mode is waiting forever; the backstop is what bounds it.
    tracker = UtteranceTracker()
    tracker.feed(final("Просто скажи щось"))
    assert tracker.utterance_end() == "Просто скажи щось"


def test_the_backstop_on_nothing_resolves_to_nothing() -> None:
    assert UtteranceTracker().utterance_end() is None


def test_an_empty_utterance_is_not_an_error() -> None:
    # v1.2's press-and-hold produces a zero-byte utterance on a very short hold. The right answer
    # to silence is silence.
    tracker = UtteranceTracker()
    tracker.feed(interim(""))
    assert tracker.finish() is None


def test_several_sentences_accumulate() -> None:
    tracker = UtteranceTracker()
    assert tracker.feed(final("Перше речення.")) == "Перше речення."
    assert tracker.feed(final("Друге речення.")) == "Друге речення."


def test_a_hold_then_a_punctuated_continuation_resolves_once() -> None:
    tracker = UtteranceTracker()
    tracker.feed(interim("я хотів"))
    tracker.feed(final("Я хотів"))
    tracker.feed(interim("сказати"))
    assert tracker.feed(final("сказати дещо.")) == "Я хотів сказати дещо."


def test_the_partial_shows_the_held_phrase_and_the_live_interim() -> None:
    # What the device renders while listening: everything settled so far, plus the current guess.
    tracker = UtteranceTracker()
    tracker.feed(final("Я хотів"))
    tracker.feed(interim("сказати"))
    assert tracker.partial == "Я хотів сказати"


def test_resolving_clears_the_tracker_for_the_next_utterance() -> None:
    # Without this the second utterance carries the first, and every turn grows.
    tracker = UtteranceTracker()
    tracker.feed(final("Перше."))
    assert tracker.feed(final("Друге.")) == "Друге."
    assert tracker.partial == ""


def test_whitespace_only_finals_are_ignored() -> None:
    tracker = UtteranceTracker()
    assert tracker.feed(final("   ")) is None
    assert not tracker.pending


def test_an_ellipsis_counts_as_a_terminator() -> None:
    tracker = UtteranceTracker()
    assert tracker.feed(final("Ну…")) == "Ну…"


# ---------------------------------------------------------------------------------------
# An empty final must not discard a good draft (v1.4)
# ---------------------------------------------------------------------------------------


def test_an_empty_final_keeps_the_most_confident_draft() -> None:
    # Observed against Deepgram, on audio the same vendor transcribes perfectly in batch mode:
    # it publishes the sentence as an interim and then closes the span with an empty final. Taking
    # only finals -- otherwise exactly right, since interims are drafts -- answers silence to
    # someone who spoke clearly.
    tracker = UtteranceTracker()
    tracker.feed(ASRChunk(text="Як", is_final=False, confidence=0.57))
    tracker.feed(ASRChunk(text="Як тебе звати?", is_final=False, confidence=0.90))
    assert tracker.finish() == "Як тебе звати?"


def test_the_draft_is_chosen_by_confidence_not_by_length() -> None:
    # Length alone would happily prefer a long mishearing to a short correct one.
    tracker = UtteranceTracker()
    tracker.feed(ASRChunk(text="Привіт!", is_final=False, confidence=0.95))
    tracker.feed(ASRChunk(text="при віт як с прави там", is_final=False, confidence=0.20))
    assert tracker.finish() == "Привіт!"


def test_a_real_final_still_wins_over_any_draft() -> None:
    # The fallback is a last resort, never a competitor: a settled final is the vendor saying it
    # will not change its mind, and a draft must never override it.
    tracker = UtteranceTracker()
    tracker.feed(ASRChunk(text="чернетка яка не збулася", is_final=False, confidence=0.99))
    assert tracker.feed(ASRChunk(text="Привіт, як справи?", is_final=True)) == "Привіт, як справи?"


def test_silence_still_resolves_to_nothing() -> None:
    # The fallback must not invent an utterance where there was none -- v1.2's press-and-hold
    # produces zero-byte utterances routinely, and the right answer to silence is silence.
    tracker = UtteranceTracker()
    assert tracker.finish() is None


def test_a_draft_does_not_survive_into_the_next_utterance() -> None:
    tracker = UtteranceTracker()
    tracker.feed(ASRChunk(text="перша фраза", is_final=False, confidence=0.9))
    assert tracker.finish() == "перша фраза"
    assert tracker.finish() is None

"""The phrase splitter, fed the way a real stream feeds it.

Every test here pushes the text through **character by character** unless it is specifically about
delta shape. That is not pedantry: the splitter's hardest case is a boundary arriving as the last
character of a delta, and a test that feeds whole sentences never produces one.
"""

from __future__ import annotations

import pytest
from roboface_server.sentence import PhraseSplitter


def feed_by_char(splitter: PhraseSplitter, text: str) -> list[str]:
    """Push `text` one character at a time, collecting everything emitted."""
    out: list[str] = []
    for char in text:
        out.extend(splitter.feed(char))
    return out


def test_a_sentence_is_emitted_once_its_boundary_is_followed_by_space() -> None:
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Привіт. ") == ["Привіт."]


def test_a_boundary_at_the_end_of_the_buffer_waits() -> None:
    # The period is the last thing we have. It may be a decimal point whose digit has not arrived,
    # so emitting now risks speaking half a number.
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Привіт.") == []
    assert splitter.pending == "Привіт."


def test_a_decimal_point_does_not_split() -> None:
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Це коштує 3.5 гривні. ") == ["Це коштує 3.5 гривні."]


def test_several_sentences_come_out_in_order() -> None:
    splitter = PhraseSplitter()
    emitted = feed_by_char(splitter, "Перше. Друге! Третє? ")
    assert emitted == ["Перше.", "Друге!", "Третє?"]


def test_a_clause_boundary_also_emits() -> None:
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Спершу одне; потім інше. ") == ["Спершу одне;", "потім інше."]


def test_an_ellipsis_emits() -> None:
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Ну… гаразд. ") == ["Ну…", "гаразд."]


@pytest.mark.parametrize("abbreviation", ["напр.", "т.д.", "вул.", "ст.", "млн."])
def test_known_abbreviations_do_not_split(abbreviation: str) -> None:
    splitter = PhraseSplitter()
    emitted = feed_by_char(splitter, f"Слово {abbreviation} далі. ")
    assert emitted == [f"Слово {abbreviation} далі."]


def test_an_abbreviation_does_not_fire_inside_a_longer_word() -> None:
    # "віст." ends with "ст." but is not the abbreviation, so this *should* split.
    splitter = PhraseSplitter()
    assert feed_by_char(splitter, "Це віст. ") == ["Це віст."]


def test_a_runaway_clause_is_flushed_at_the_limit() -> None:
    splitter = PhraseSplitter(max_chars=40)
    text = "слово " * 20  # 120 chars, no punctuation at all
    emitted = feed_by_char(splitter, text)
    assert emitted, "a model that never punctuates must still be spoken"
    for phrase in emitted:
        assert len(phrase) <= 40


def test_a_forced_flush_never_cuts_a_word_in_half() -> None:
    splitter = PhraseSplitter(max_chars=20)
    emitted = feed_by_char(splitter, "альфа бета гама дельта епсилон дзета ")
    for phrase in emitted:
        # Every emitted phrase is whole words: rejoining with single spaces reproduces the input's
        # words exactly, in order.
        assert not phrase.startswith(" ") and not phrase.endswith(" ")
    words = " ".join(emitted).split() + splitter.pending.split()
    assert words == "альфа бета гама дельта епсилон дзета".split()


def test_one_unbroken_word_longer_than_the_limit_is_left_whole() -> None:
    # There is no word boundary to cut on, and cutting anyway would split the word. It is spoken
    # when it ends, which for real text is very soon.
    splitter = PhraseSplitter(max_chars=10)
    assert feed_by_char(splitter, "невідповідальність") == []
    assert splitter.pending == "невідповідальність"


def test_flush_returns_the_tail() -> None:
    splitter = PhraseSplitter()
    feed_by_char(splitter, "Коротка відповідь")
    assert splitter.flush() == "Коротка відповідь"


def test_flush_on_an_empty_buffer_is_none() -> None:
    splitter = PhraseSplitter()
    assert splitter.flush() is None
    feed_by_char(splitter, "Привіт. ")
    assert splitter.flush() is None


def test_nothing_is_lost_between_feed_and_flush() -> None:
    # The property that matters for the whole phase: everything the model wrote is eventually
    # spoken, once, in order.
    text = "Перше речення. Друге; з комою. І хвіст без крапки"
    splitter = PhraseSplitter()
    emitted = feed_by_char(splitter, text)
    tail = splitter.flush()
    if tail:
        emitted.append(tail)
    assert " ".join(emitted).split() == text.split()


def test_delta_boundaries_do_not_change_the_result() -> None:
    # The same text split into different delta shapes must produce the same phrases -- a real
    # stream chooses its own chunking and the splitter cannot depend on it.
    text = "Перше речення. Друге речення! Третє? "
    by_char = feed_by_char(PhraseSplitter(), text)

    chunked = PhraseSplitter()
    out: list[str] = []
    for size in (7, 3, 11, 5):
        while text:
            out.extend(chunked.feed(text[:size]))
            text = text[size:]
    assert out == by_char

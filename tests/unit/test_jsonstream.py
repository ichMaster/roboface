"""The incremental JSON reader.

Every test here is really the same test: **a chunk boundary may fall anywhere, and the result must
not depend on where it fell.** That is not a hypothetical — the SDK hands over whatever the network
delivered, and this server's replies are Ukrainian, which is two bytes a letter and frequently
arrives as `\\uXXXX` escapes. A boundary in the middle of one is the ordinary case.

So the workhorse below feeds the same document at every chunk size from one character upward and
asserts the events are identical. A reader that is correct only at the sizes someone thought to try
is a reader that fails in production on the day a reply gets longer.
"""

from __future__ import annotations

import json

import pytest
from roboface_server.jsonstream import Field, MalformedJson, ObjectStream, TextDelta


def _read(
    document: str, *, chunk: int, stream_keys: frozenset[str] = frozenset({"reply"})
) -> tuple[dict[str, object], str]:
    """Feed `document` in `chunk`-sized pieces; return the scalars and the reassembled text."""
    reader = ObjectStream(stream_keys)
    events: list[object] = []
    for index in range(0, len(document), chunk):
        events += reader.feed(document[index : index + chunk])
    events += reader.finish()

    fields = {event.name: event.value for event in events if isinstance(event, Field)}
    text = "".join(event.text for event in events if isinstance(event, TextDelta))
    return fields, text


#: One character at a time is every boundary at once; the large sizes are the realistic ones.
CHUNK_SIZES = [1, 2, 3, 5, 8, 13, 64, 4096]


# --------------------------------------------------------------------------------------
# The shape this reader exists for
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize("chunk", CHUNK_SIZES)
def test_the_v2_2_response_reads_identically_at_every_chunk_size(chunk: int) -> None:
    reply = "Привіт! Я тут і слухаю тебе."
    document = json.dumps(
        {"emotion": "joy", "intensity": 0.8, "reply": reply}, ensure_ascii=False
    )

    fields, text = _read(document, chunk=chunk)

    assert fields == {"emotion": "joy", "intensity": 0.8}
    assert text == reply


@pytest.mark.parametrize("chunk", CHUNK_SIZES)
def test_escaped_unicode_reads_identically_at_every_chunk_size(chunk: int) -> None:
    r"""`ensure_ascii=True`, so every Cyrillic letter is a six-character `\uXXXX` escape.

    This is the shape the real SDK is most likely to deliver, and the one where a boundary lands
    mid-escape on nearly every chunk.
    """
    reply = "Привіт! Це «тест» — з переносом."
    document = json.dumps({"emotion": "calm", "intensity": 0.5, "reply": reply})

    fields, text = _read(document, chunk=chunk)

    assert fields == {"emotion": "calm", "intensity": 0.5}
    assert text == reply


@pytest.mark.parametrize("chunk", CHUNK_SIZES)
def test_a_surrogate_pair_survives_a_boundary_between_its_halves(chunk: int) -> None:
    """JSON spells an astral character as two escapes. Emitting the first alone produces a lone
    surrogate, which cannot be encoded and would fail somewhere far from here."""
    reply = "Готово 😀 і ще 🎉"
    document = json.dumps({"emotion": "joy", "intensity": 1.0, "reply": reply})

    _fields, text = _read(document, chunk=chunk)

    assert text == reply


@pytest.mark.parametrize("chunk", CHUNK_SIZES)
def test_every_json_escape_decodes(chunk: int) -> None:
    reply = 'a\nb\tc"d\\e/f\bg\fh\ri'
    document = json.dumps({"emotion": "neutral", "intensity": 0.0, "reply": reply})

    _fields, text = _read(document, chunk=chunk)

    assert text == reply


# --------------------------------------------------------------------------------------
# Streaming, rather than merely parsing
# --------------------------------------------------------------------------------------


def test_the_scalars_arrive_before_the_text_they_describe() -> None:
    """The whole reason this class exists rather than `json.loads`.

    The report is complete after the head of the object, which is long before the reply is
    finished — so the face can change as the device begins to speak.
    """
    reader = ObjectStream(frozenset({"reply"}))
    head = reader.feed('{"emotion": "sad", "intensity": 0.3, "reply": "Пр')

    assert [event for event in head if isinstance(event, Field)] == [
        Field("emotion", "sad"),
        Field("intensity", 0.3),
    ]
    assert [event for event in head if isinstance(event, TextDelta)] == [TextDelta("reply", "Пр")]


def test_text_is_batched_per_chunk_not_per_character() -> None:
    """Per-character deltas would each become a `reply` frame on the wire: a 400-character answer
    would be 400 websocket frames instead of the dozen the network actually delivered.

    Nothing is delayed by the batching — the chunk is the unit in which the text arrived.
    """
    reader = ObjectStream(frozenset({"reply"}))
    events = reader.feed('{"emotion": "joy", "intensity": 1, "reply": "abcdefghij"}')

    assert [event for event in events if isinstance(event, TextDelta)] == [
        TextDelta("reply", "abcdefghij")
    ]


def test_a_streamed_key_is_not_also_reported_as_a_field() -> None:
    """Its value was already delivered. Reporting it twice would make "did the reply arrive"
    ambiguous, and would buffer the whole reply to do it."""
    _fields, text = _read(
        '{"emotion": "joy", "intensity": 1, "reply": "hello"}', chunk=4096
    )
    fields, _text = _read('{"emotion": "joy", "intensity": 1, "reply": "hello"}', chunk=4096)

    assert "reply" not in fields
    assert text == "hello"


def test_an_unstreamed_string_is_buffered_and_reported_whole() -> None:
    fields, text = _read(
        '{"emotion": "joy", "intensity": 1, "reply": "hi"}', chunk=3, stream_keys=frozenset()
    )

    assert fields["reply"] == "hi"
    assert text == ""


# --------------------------------------------------------------------------------------
# Values other than strings
# --------------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("literal", "expected"),
    [("0.8", 0.8), ("1", 1), ("0", 0), ("-0.25", -0.25), ("true", True), ("false", False),
     ("null", None), ("1e-3", 0.001)],
    ids=["float", "int", "zero", "negative", "true", "false", "null", "exponent"],
)
@pytest.mark.parametrize("chunk", [1, 4096])
def test_scalar_values_decode_as_json_does(literal: str, expected: object, chunk: int) -> None:
    fields, _text = _read(f'{{"intensity": {literal}, "reply": ""}}', chunk=chunk)

    assert fields["intensity"] == expected


def test_whitespace_between_tokens_is_ignored() -> None:
    fields, text = _read(
        '  {\n  "emotion" :  "calm" ,\n  "intensity" : 0.5 ,\n  "reply" : "hi"\n}  ', chunk=1
    )

    assert fields == {"emotion": "calm", "intensity": 0.5}
    assert text == "hi"


# --------------------------------------------------------------------------------------
# Failure, and why it is a failure here rather than a coercion
# --------------------------------------------------------------------------------------


def test_a_truncated_document_raises_rather_than_returning_a_short_reply() -> None:
    """The silent version of this bug is the dangerous one: a caller would see a short answer and
    no error, which is indistinguishable from a model that was brief."""
    reader = ObjectStream(frozenset({"reply"}))
    reader.feed('{"emotion": "joy", "intensity": 1, "reply": "half a sen')

    with pytest.raises(MalformedJson):
        reader.finish()


def test_a_response_that_is_not_an_object_raises() -> None:
    reader = ObjectStream(frozenset({"reply"}))

    with pytest.raises(MalformedJson):
        reader.feed("I'm afraid I can't do that.")


def test_an_unknown_escape_raises() -> None:
    reader = ObjectStream(frozenset())

    with pytest.raises(MalformedJson):
        reader.feed('{"reply": "a\\qb"}')


def test_a_lone_surrogate_raises_rather_than_producing_unencodable_text() -> None:
    reader = ObjectStream(frozenset({"reply"}))

    with pytest.raises(MalformedJson):
        reader.feed('{"reply": "\\ud83d"}')


def test_trailing_content_after_the_object_raises() -> None:
    reader = ObjectStream(frozenset())
    reader.feed('{"reply": "hi"}')

    with pytest.raises(MalformedJson):
        reader.feed("{}")


def test_finishing_a_complete_document_is_quiet() -> None:
    reader = ObjectStream(frozenset({"reply"}))
    reader.feed('{"emotion": "joy", "intensity": 1, "reply": "hi"}')

    assert reader.finish() == []

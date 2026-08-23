"""M5-002 — the frame contract and its two guards.

The guards matter more than the schema. A frame over the write limit takes two writes
instead of one and silently halves the refresh rate; a stray ``·`` renders as an empty
box on a font with no glyph for it. Neither shows up in a test that is not looking for
it, and neither shows up anywhere but on the panel.
"""

from __future__ import annotations

from typing import Any

import pytest

from bridge import devices, frames

# One valid example per want. Real shapes, real sizes — these are what M5-003 must
# produce and what the firmware will parse.
EXAMPLES: dict[int, dict[str, Any]] = {
    devices.WANT_NOTIFY: {
        "v": 1, "s": 0, "next": 5, "dim": 100,
        "n": [{"k": "retry", "t": "RF-086 x4", "b": 2, "g": 4}],
    },
    devices.SCREEN_NOW: {
        "v": 1, "s": 1, "next": 15, "dim": 100, "st": "run",
        "cur": "v05.03 RF-112", "stp": "execute-issues", "el": "04:25",
        "ct": "2m", "cc": 1, "pct": 91, "idit": "42/46", "eta": "30-60 min",
    },
    devices.SCREEN_VELOCITY: {
        "v": 1, "s": 2, "next": 120, "dim": 100,
        "sp": "7511232", "now": "6/h", "med": "1:42", "left": 4,
    },
    devices.SCREEN_PLAN: {
        "v": 1, "s": 3, "next": 60, "dim": 100,
        "vs": "##############>", "done": "14/15",
    },
    devices.SCREEN_FRICTION: {
        "v": 1, "s": 4, "next": 60, "dim": 100, "fp": "76%", "rt": "10/42",
        "pct": 24, "top": ["RF-086 x4"], "fnd": [0, 6, 3], "open": "v01.02 MEDIUM",
    },
    devices.SCREEN_ANALYTICS: {
        "v": 1, "s": 5, "next": 60, "dim": 100,
        "st": [[6, 312, 41], [7, 49, 26], [7, 135, 16], [7, 30, 10], [7, 53, 7]],
        "tp": "238", "tps": "1235667", "cov": 42,
    },
    devices.SCREEN_BURNDOWN: {
        "v": 1, "s": 6, "next": 60, "dim": 100,
        "bd": "4646464633292524242222201714111007050404",
        "tot": 46, "est": 58, "rem": 4, "el": 15928, "lo": 1200, "hi": 1800, "err": 26,
    },
}

IDS = [devices.WANT_NAMES[w] for w in EXAMPLES]


# ── the schema ───────────────────────────────────────────────────────────────


@pytest.mark.parametrize("want", list(EXAMPLES), ids=IDS)
def test_every_frame_type_has_a_valid_example(want: int) -> None:
    assert frames.validate(EXAMPLES[want]) == []


@pytest.mark.parametrize("want", list(EXAMPLES), ids=IDS)
def test_removing_any_required_key_is_reported_by_name(want: int) -> None:
    """Naming the key is the point — "invalid frame" sends you reading the schema.

    ``s`` is excluded: it is the discriminator, so its absence is reported by the
    early routing check below rather than as one missing key among many. There is
    nothing to validate a frame *against* until you know which frame it is.
    """
    keys = [k for k in frames.COMMON + frames.REQUIRED[want] if k != "s"]
    for key in keys:
        broken = {k: v for k, v in EXAMPLES[want].items() if k != key}
        problems = frames.validate(broken)
        assert any(repr(key) in p for p in problems), f"{key} removed but not reported"


@pytest.mark.parametrize("want", list(EXAMPLES), ids=IDS)
def test_removing_the_screen_id_is_reported_as_a_routing_failure(want: int) -> None:
    broken = {k: v for k, v in EXAMPLES[want].items() if k != "s"}
    assert frames.validate(broken) == ["unknown or missing screen id: s=None"]


def test_an_unknown_screen_id_is_rejected_before_anything_else() -> None:
    assert frames.validate({"v": 1, "s": 99}) == ["unknown or missing screen id: s=99"]


def test_a_frame_that_is_not_an_object_is_rejected() -> None:
    assert frames.validate([1, 2, 3])[0].startswith("frame must be an object")


def test_a_wrong_frame_version_is_reported() -> None:
    wrong = {**EXAMPLES[devices.SCREEN_NOW], "v": 2}
    assert any("v must be 1" in p for p in frames.validate(wrong))


# ── guard 1: one write ───────────────────────────────────────────────────────


def test_the_limit_is_documented_where_it_is_defined() -> None:
    """A bare ``182`` would be a number nobody could safely change.

    Asserted against the source, because the reasoning lives in a ``#:`` comment that
    no runtime attribute exposes — and a comment is exactly the thing a refactor
    drops.
    """
    from pathlib import Path

    source = (Path(frames.__file__).read_text())
    limit_block = source.split("MAX_FRAME_BYTES")[0].rsplit("#: Maximum bytes", 1)
    assert len(limit_block) == 2, "the constant lost its explanatory comment"
    reasoning = limit_block[1]
    assert "CoreBluetooth" in reasoning
    assert "M5-014" in reasoning, "the verification task must stay named"
    assert "response=True" in reasoning, "the fallback must stay documented"


def test_fits_accepts_exactly_the_limit_and_rejects_one_byte_more() -> None:
    at_limit = {"v": 1, "s": 3, "next": 60, "dim": 100, "vs": "", "done": ""}
    pad = frames.MAX_FRAME_BYTES - frames.size(at_limit)
    at_limit["vs"] = "x" * pad
    assert frames.size(at_limit) == frames.MAX_FRAME_BYTES
    assert frames.fits(at_limit)

    over = {**at_limit, "vs": "x" * (pad + 1)}
    assert frames.size(over) == frames.MAX_FRAME_BYTES + 1
    assert not frames.fits(over)
    assert any("over the" in p for p in frames.validate(over))


@pytest.mark.parametrize("want", list(EXAMPLES), ids=IDS)
def test_every_example_travels_in_one_write(want: int) -> None:
    assert frames.fits(EXAMPLES[want]), (
        f"{devices.WANT_NAMES[want]} is {frames.size(EXAMPLES[want])} B"
    )


def test_the_largest_example_still_leaves_headroom() -> None:
    """NOW is the tightest frame in the design; the margin is worth watching."""
    largest = max(frames.size(f) for f in EXAMPLES.values())
    assert largest == frames.size(EXAMPLES[devices.SCREEN_NOW])
    assert 0 < frames.MAX_FRAME_BYTES - largest < 40


# ── guard 2: ASCII only ──────────────────────────────────────────────────────


@pytest.mark.parametrize("char", ["·", "–", "×", "█", "●"])
def test_the_characters_review_actually_caught_are_rejected(char: str) -> None:
    """Middle dot, en dash and multiplication sign each reached a draft frame; the
    block and the bullet are what a renderer would reach for if the frame carried
    glyphs instead of numbers."""
    frame = {**EXAMPLES[devices.SCREEN_PLAN], "done": char}
    assert not frames.is_ascii(frame)
    assert any("non-ASCII" in p for p in frames.validate(frame))


def test_non_ascii_hiding_in_a_nested_value_is_still_caught() -> None:
    """Checked over the serialised form, so it cannot hide inside a list."""
    frame = {**EXAMPLES[devices.SCREEN_FRICTION], "top": ["RF-086 ×4"]}
    assert not frames.is_ascii(frame)


def test_non_ascii_in_a_key_is_still_caught() -> None:
    frame: dict[str, Any] = {**EXAMPLES[devices.SCREEN_PLAN], "café": 1}
    assert not frames.is_ascii(frame)


@pytest.mark.parametrize("want", list(EXAMPLES), ids=IDS)
def test_every_example_is_pure_ascii(want: int) -> None:
    assert frames.is_ascii(EXAMPLES[want])


# ── notifications ────────────────────────────────────────────────────────────


def test_more_than_three_notifications_do_not_fit_one_write() -> None:
    item = {"k": "release", "t": "v05.03 tagged", "b": 0}
    frame = {**EXAMPLES[devices.WANT_NOTIFY], "n": [item] * 4}
    assert any("at most" in p for p in frames.validate(frame))


def test_three_notifications_are_allowed_and_still_fit() -> None:
    item = {"k": "release", "t": "v05.03 tagged", "b": 0}
    frame = {**EXAMPLES[devices.WANT_NOTIFY], "n": [item] * 3}
    assert frames.validate(frame) == []


def test_an_empty_queue_is_valid_and_is_the_common_case() -> None:
    """57 notifications across 4.1 hours against a 5 s poll: silence is normal."""
    frame = {**EXAMPLES[devices.WANT_NOTIFY], "n": []}
    assert frames.validate(frame) == []
    assert frames.size(frame) < 50


@pytest.mark.parametrize("bad", [-1, 4, "2", None])
def test_a_volume_outside_the_scale_is_rejected(bad: Any) -> None:
    frame = {**EXAMPLES[devices.WANT_NOTIFY], "n": [{"k": "x", "t": "y", "b": bad}]}
    assert any("b must be one of" in p for p in frames.validate(frame))


def test_g_must_be_absent_rather_than_null() -> None:
    """Absent saves a byte on every answer, and the two are not the same contract."""
    frame = {**EXAMPLES[devices.WANT_NOTIFY], "n": [{"k": "x", "t": "y", "b": 0, "g": None}]}
    assert any("absent, never null" in p for p in frames.validate(frame))


def test_an_unexpected_key_in_a_notification_is_reported() -> None:
    frame = {
        **EXAMPLES[devices.WANT_NOTIFY],
        "n": [{"k": "x", "t": "y", "b": 0, "colour": "red"}],
    }
    assert any("unexpected keys" in p for p in frames.validate(frame))


# ── the two pacing fields ────────────────────────────────────────────────────


@pytest.mark.parametrize("bad", [0, -5, "15", None, True])
def test_next_must_be_a_positive_number_of_seconds(bad: Any) -> None:
    frame = {**EXAMPLES[devices.SCREEN_NOW], "next": bad}
    assert any("next must be" in p for p in frames.validate(frame))


@pytest.mark.parametrize("bad", [-1, 101, "50", None, True])
def test_dim_must_be_a_percentage(bad: Any) -> None:
    frame = {**EXAMPLES[devices.SCREEN_NOW], "dim": bad}
    assert any("dim must be" in p for p in frames.validate(frame))


def test_every_frame_carries_next_and_dim_because_the_device_holds_no_policy() -> None:
    for want, example in EXAMPLES.items():
        assert "next" in example and "dim" in example, devices.WANT_NAMES[want]


# ── the doc and the code must not drift ──────────────────────────────────────


def test_the_schema_covers_exactly_the_wants_the_vision_doc_names() -> None:
    """A screen added to the doc and forgotten here would be caught by nothing else."""
    from pathlib import Path

    vision = (Path(__file__).resolve().parent.parent / "device-frontends-vision.md").read_text()
    for want, name in devices.WANT_NAMES.items():
        assert want in frames.REQUIRED, f"{name} missing from REQUIRED"
        expected = "notifications" if want == devices.WANT_NOTIFY else name.upper()
        assert expected in vision, f"{name} not described in the vision doc"
    assert set(frames.REQUIRED) == set(devices.WANT_NAMES)


def test_the_frame_version_is_independent_of_the_event_schema_version() -> None:
    """Architecture §11.1: two fields named ``v``, different wires, different owners.

    They happen to both be 1 today, so comparing the values proves nothing. What is
    worth pinning is that the frame version is not *derived* from the event one —
    wiring them together would mean bumping the log schema implied a reflash, and
    reshaping a screen implied a log migration.
    """
    from pathlib import Path

    source = Path(frames.__file__).read_text()
    assert "FRAME_VERSION = 1" in source, "must be its own literal, not a reference"
    assert "SCHEMA_VERSION" not in source, (
        "bridge/frames.py reads the event schema version; the two must stay unrelated"
    )
    assert "architecture §11.1" in source or "§11.1" in source

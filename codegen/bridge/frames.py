"""The frame contract — seven shapes, and the two guards that keep them shippable.

This is what the firmware parses, so it lands before anything produces a frame. The
prose owner is device-frontends-vision.md §2.3; this module is its executable form.

Hand-rolled validation, matching ``tracker/schema.py``'s approach. Not for the same
reason — the bridge is allowed third-party imports — but because the schema is small,
the shapes are flat, and a dependency here would buy nothing.

**The two guards are the load-bearing part.** Both are one line, and both catch a class
of defect that would otherwise only appear on a panel nobody is looking at:

``fits``    a frame over the write limit silently halves the refresh rate, because it
            takes two writes instead of one.
``is_ascii`` a stray ``·`` renders as an empty box on a font that has no glyph for it,
            and only on hardware — never in a test, never in the prototype.
"""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from typing import Any

from bridge import devices

#: Frame schema version. **Unrelated to the event schema's ``v``** — different wire,
#: different owner, independent evolution (architecture §11.1). Bumping one never
#: obliges the other.
FRAME_VERSION = 1

#: Maximum bytes in one BLE write.
#:
#: macOS CoreBluetooth negotiates an ATT MTU of roughly 185, leaving 182 bytes of
#: payload for write-without-response — far below the 512 an ESP32 accepts. Derived
#: from ``bleak``'s CoreBluetooth backend, which computes
#: ``maximumWriteValueLengthForType_(WriteWithoutResponse) + 3``.
#:
#: **Unverified against real hardware.** M5-014's first job is to print
#: ``client.mtu_size - 3`` and compare. If it comes back lower, the fallback is one
#: argument — ``write_gatt_char(..., response=True)`` uses long writes and carries up
#: to 512 at the cost of a round trip, which at these rates is irrelevant.
MAX_FRAME_BYTES = 182

#: Keys every answer carries, whatever it is answering.
#:
#: ``next`` and ``dim`` are here because the device holds no policy: it obeys the last
#: numbers it was given (vision §4.2, §4.4).
COMMON = ("v", "s", "next", "dim")

#: Required keys per ``want``, beyond :data:`COMMON`.
REQUIRED: Mapping[int, tuple[str, ...]] = {
    devices.WANT_NOTIFY: ("n",),
    devices.SCREEN_NOW: ("st", "cur", "stp", "el", "ct", "cc", "pct", "idit", "eta"),
    devices.SCREEN_VELOCITY: ("sp", "now", "med", "left"),
    devices.SCREEN_PLAN: ("vs", "done"),
    devices.SCREEN_FRICTION: ("fp", "rt", "pct", "top", "fnd", "open"),
    devices.SCREEN_ANALYTICS: ("st", "tp", "tps", "cov"),
    devices.SCREEN_BURNDOWN: ("bd", "tot", "est", "rem", "el", "lo", "hi", "err"),
}

#: Keys a notification item may carry. ``g`` is optional and **absent, never null**,
#: when there is nothing to navigate to — a byte saved on every answer.
NOTIFY_ITEM_REQUIRED = ("k", "t", "b")
NOTIFY_ITEM_OPTIONAL = ("g",)

#: Volume, not category. Alerts and events are one list; ``b`` is all that separates
#: them, and it governs sound and haptics only — never the backlight (vision §5.1).
VOLUME_LEVELS = (0, 1, 2, 3)

#: Most that fits one write alongside the envelope. The queue carries the rest over.
MAX_NOTIFICATIONS = 3


def encode(frame: Mapping[str, Any]) -> bytes:
    """Serialise exactly as the transport will, so a size check means something."""
    return json.dumps(frame, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def size(frame: Mapping[str, Any]) -> int:
    return len(encode(frame))


def fits(frame: Mapping[str, Any]) -> bool:
    """Whether this frame travels in a single BLE write."""
    return size(frame) <= MAX_FRAME_BYTES


def is_ascii(frame: Mapping[str, Any]) -> bool:
    """Whether the firmware's stock Latin font can render every character in it.

    Checked over the serialised form rather than field by field, so a non-ASCII
    character cannot hide inside a nested list or a dict key.
    """
    return encode(frame).decode("utf-8").isascii()


def validate(frame: Any) -> list[str]:
    """Problems with ``frame``, as strings. Empty means valid.

    Returns rather than raises, matching ``tracker.schema.validate``: a caller
    collecting problems can report all of them, where an exception reports one.
    """
    problems: list[str] = []
    if not isinstance(frame, dict):
        return [f"frame must be an object, got {type(frame).__name__}"]

    want = frame.get("s")
    if not isinstance(want, int) or want not in REQUIRED:
        return [f"unknown or missing screen id: s={want!r}"]

    if frame.get("v") != FRAME_VERSION:
        problems.append(f"v must be {FRAME_VERSION}, got {frame.get('v')!r}")

    for key in COMMON + REQUIRED[want]:
        if key not in frame:
            problems.append(f"{devices.WANT_NAMES[want]}: missing required key {key!r}")

    problems.extend(_dim_problems(frame))
    problems.extend(_next_problems(frame))
    if want == devices.WANT_NOTIFY:
        problems.extend(_notify_problems(frame))

    if not fits(frame):
        problems.append(f"frame is {size(frame)} B, over the {MAX_FRAME_BYTES} B limit")
    if not is_ascii(frame):
        problems.append("frame contains non-ASCII; the panel font cannot render it")
    return problems


def _dim_problems(frame: Mapping[str, Any]) -> list[str]:
    dim = frame.get("dim")
    if "dim" not in frame:
        return []
    if not isinstance(dim, int) or isinstance(dim, bool) or not 0 <= dim <= 100:
        return [f"dim must be a percentage 0-100, got {dim!r}"]
    return []


def _next_problems(frame: Mapping[str, Any]) -> list[str]:
    nxt = frame.get("next")
    if "next" not in frame:
        return []
    if not isinstance(nxt, int) or isinstance(nxt, bool) or nxt <= 0:
        return [f"next must be a positive number of seconds, got {nxt!r}"]
    return []


def _notify_problems(frame: Mapping[str, Any]) -> list[str]:
    items = frame.get("n")
    if not isinstance(items, Sequence) or isinstance(items, (str, bytes)):
        return [f"n must be a list of notifications, got {type(items).__name__}"]

    problems: list[str] = []
    if len(items) > MAX_NOTIFICATIONS:
        problems.append(
            f"n carries {len(items)} notifications; at most "
            f"{MAX_NOTIFICATIONS} fit one write"
        )
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            problems.append(f"n[{index}] must be an object")
            continue
        for key in NOTIFY_ITEM_REQUIRED:
            if key not in item:
                problems.append(f"n[{index}]: missing required key {key!r}")
        if "b" in item and item["b"] not in VOLUME_LEVELS:
            problems.append(f"n[{index}]: b must be one of {VOLUME_LEVELS}, got {item['b']!r}")
        if item.get("g", 1) is None:
            problems.append(f"n[{index}]: g must be absent, never null")
        unknown = set(item) - set(NOTIFY_ITEM_REQUIRED) - set(NOTIFY_ITEM_OPTIONAL)
        if unknown:
            problems.append(f"n[{index}]: unexpected keys {sorted(unknown)}")
    return problems

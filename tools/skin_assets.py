#!/usr/bin/env python3
"""Turn the drawn PNGs into something the firmware can read without a decoder.

    .venv/bin/python tools/skin_assets.py

**The decoding happens here, once, on a machine with memory to spare.** The device gets a flat
binary it can read directly out of memory-mapped flash: no PNG decoder linked in, no decode at boot,
no PSRAM copy of anything that never changes. That is worth more than the disk space it costs, and
it is why this file exists rather than `drawPng` being called on the board.

Three encodings, each chosen by what the firmware does with the pixels:

* **Opaque bodies** (`stackchan`, `ghost`) → RGB565. Pushed straight through; nothing to compute.
* **Tinted bodies** (`flame`, `jelly`, `cloud`) → 8-bit **luminance**. Their element *is* their
  colour, so the firmware multiplies one tint across the whole body per emotion. Storing them in
  colour would mean throwing that colour away on every frame, and would double the flash they take.
* **Features and overlays** → 8-bit alpha, or RGB565 + alpha. The eyes and mouths are white on
  transparency by design: only the alpha carries shape, and the ink comes from the skin. That is
  what lets one set of nineteen images serve all five faces.

The output is one `.bin` and one generated header of offsets. A binary rather than C arrays because
885 KB of hex text is five megabytes of source, and a repository should not carry that to say the
same thing.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "specification/features/skin-assets/assets"
OUT_BIN = ROOT / "firmware/src/assets/skin_assets.bin"
OUT_HEADER = ROOT / "firmware/src/assets/skin_assets.h"

#: In the order `roboface::Emotion` declares them. The firmware indexes by the enum, so a mismatch
#: here would show as a face wearing the wrong mouth -- and only for one emotion, which is the kind
#: nobody notices for a week.
EMOTIONS = ["neutral", "calm", "joy", "thinking", "surprised", "sad", "error"]

#: In the order `roboface::MouthFrame` declares them, minus `closed`, which reuses `mouth-neutral`.
VISEMES = ["ajar", "half", "wide", "open"]

#: In the order `skinAt()` returns them.
SKINS = ["stackchan", "ghost", "flame", "jelly", "cloud"]
TINTED = {"flame", "jelly", "cloud"}

OVERLAYS = ["ghost-blush", "ghost-tear", "jelly-glow", "cloud-sun", "cloud-rain"]


def rgb565(r: int, g: int, b: int) -> int:
    """The panel's own colour, and why the spec told the artist to quantise before judging."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def encode_rgb565(image: Image.Image) -> bytes:
    rgba = image.convert("RGBA")
    raw = rgba.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 4):
        out += struct.pack("<H", rgb565(raw[i], raw[i + 1], raw[i + 2]))
    return bytes(out)


def encode_luminance(image: Image.Image) -> bytes:
    """Rec. 601 luma, which is what "shading carried by lightness" means numerically.

    The three tinted bodies were drawn as neutral mid-tone forms precisely so this is lossless in
    the way that matters: the shape survives, and the hue was never theirs to keep.
    """
    return image.convert("RGBA").convert("L").tobytes()


def encode_alpha(image: Image.Image) -> bytes:
    return image.convert("RGBA").getchannel("A").tobytes()


def encode_rgba(image: Image.Image) -> bytes:
    """RGB565 + a separate alpha plane, rather than interleaved.

    Two planes because the firmware reads them at different times: the alpha decides whether a pixel
    is touched at all, and most overlay pixels are transparent. Skipping a run of zeroes in a byte
    plane is a cheaper question than unpacking three bytes to ask it.
    """
    colour = encode_rgb565(image)
    return colour + encode_alpha(image)


class Blob:
    """The accumulating binary, and the offsets into it."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.entries: list[tuple[str, int, int, int, int]] = []

    def add(self, name: str, payload: bytes, width: int, height: int) -> None:
        # Aligned to four, so a `const uint16_t*` into the blob is never misaligned. The ESP32-S3
        # tolerates unaligned reads from flash and pays for them; nothing here is worth paying for.
        while len(self.data) % 4:
            self.data.append(0)
        self.entries.append((name, len(self.data), len(payload), width, height))
        self.data += payload


def main() -> int:
    if not SRC.is_dir():
        print(f"no assets at {SRC}", file=sys.stderr)
        return 1

    blob = Blob()

    def load(filename: str) -> Image.Image:
        path = SRC / filename
        if not path.is_file():
            raise SystemExit(f"missing asset: {filename}")
        return Image.open(path)

    for skin in SKINS:
        image = load(f"body-{skin}.png")
        if image.size != (320, 240):
            raise SystemExit(f"body-{skin}.png is {image.size}, expected (320, 240)")
        if skin in TINTED:
            blob.add(f"body_{skin}", encode_luminance(image), *image.size)
        else:
            blob.add(f"body_{skin}", encode_rgb565(image), *image.size)

    for emotion in EMOTIONS:
        image = load(f"eyes-{emotion}.png")
        blob.add(f"eyes_{emotion}", encode_alpha(image), *image.size)
    closed = load("eyes-closed.png")
    blob.add("eyes_closed", encode_alpha(closed), *closed.size)

    for emotion in EMOTIONS:
        image = load(f"mouth-{emotion}.png")
        blob.add(f"mouth_{emotion}", encode_alpha(image), *image.size)
    for viseme in VISEMES:
        image = load(f"mouth-{viseme}.png")
        blob.add(f"mouth_{viseme}", encode_alpha(image), *image.size)

    for overlay in OVERLAYS:
        image = load(f"elem-{overlay}.png")
        blob.add(f"elem_{overlay.replace('-', '_')}", encode_rgba(image), *image.size)

    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    OUT_BIN.write_bytes(bytes(blob.data))

    lines = [
        "// Generated by tools/skin_assets.py. Do not edit.",
        "//",
        "// Offsets into `skin_assets.bin`, which is linked in whole by `skin_assets.S` and read",
        "// straight out of memory-mapped flash. Nothing here is copied to RAM: a body is",
        "// 150 KB and never changes, and PSRAM is worth more than a copy of something the",
        "// flash already holds at an address the CPU can read.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace assets {",
        "",
        "extern const uint8_t kBlobStart[] asm(\"_binary_src_assets_skin_assets_bin_start\");",
        "",
        "struct Entry {",
        "    uint32_t offset;",
        "    uint32_t length;",
        "    uint16_t width;",
        "    uint16_t height;",
        "};",
        "",
    ]
    for name, offset, length, width, height in blob.entries:
        lines.append(
            f"inline constexpr Entry k{name.title().replace('_', '')}"
            f" = {{{offset}u, {length}u, {width}, {height}}};"
        )
    lines += [
        "",
        "inline const uint8_t* bytesOf(const Entry& entry) { return kBlobStart + entry.offset; }",
        "inline const uint16_t* pixelsOf(const Entry& entry) {",
        "    return reinterpret_cast<const uint16_t*>(kBlobStart + entry.offset);",
        "}",
        "",
        f"inline constexpr std::size_t kBlobBytes = {len(blob.data)}u;",
        "",
        "}  // namespace assets",
        "",
    ]
    OUT_HEADER.write_text("\n".join(lines))

    size_kb = len(blob.data) / 1024
    print(f"{len(blob.entries)} assets -> {OUT_BIN.relative_to(ROOT)}  ({size_kb:.0f} KB)")
    for name, offset, length, width, height in blob.entries:
        print(f"  {name:22} {width:>3}x{height:<3} {length / 1024:7.1f} KB  @{offset}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

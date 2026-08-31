// Compositing the drawn faces onto the sprite.
//
// The five skins stopped being circles and rectangles in v2.6.2. What replaced them is not a
// different renderer but a **different source of pixels**: the same `EmotionFrame` still decides
// what is shown, and the same manifest still decides where it goes. Only the shapes are now drawn
// by a person instead of by `fillCircle`.
//
// **Nothing is copied to RAM.** The blob lives in flash, which the ESP32-S3 maps into the address
// space, so a body is read where it lies. 876 KB of art costs 876 KB of flash and no PSRAM at all
// — and PSRAM is the scarce thing here, since the sprite already holds 150 KB of it.
//
// Three encodings, because the firmware asks three different questions of them:
//
//   * an **opaque body** is RGB565 and is copied
//   * a **tinted body** is 8-bit luminance and is multiplied by the emotion's colour
//   * a **feature** is 8-bit alpha and is blended in the skin's ink
//
// Glue, not pure: it writes into an `M5Canvas`. The decisions -- which image, what colour, where --
// are all made in `pure/`, and this only puts pixels where it is told.

#pragma once

#include <M5Unified.h>

#include <cstdint>

#include "assets/skin_assets.h"
#include "pure/face.h"
#include "pure/lipsync.h"
#include "pure/skin.h"

namespace app {
namespace art {

//: The body for a skin, by the index `skinAt` uses. Two shapes behind one accessor because the
//: caller does not choose the encoding -- the artist did, by drawing three of the five to be
//: recoloured.
inline const assets::Entry& bodyFor(std::size_t skin_index) {
    switch (skin_index) {
        case 1: return assets::kBodyGhost;
        case 2: return assets::kBodyFlame;
        case 3: return assets::kBodyJelly;
        case 4: return assets::kBodyCloud;
        default: return assets::kBodyStackchan;
    }
}

inline const assets::Entry& eyesFor(roboface::Emotion emotion) {
    switch (emotion) {
        case roboface::Emotion::kCalm: return assets::kEyesCalm;
        case roboface::Emotion::kJoy: return assets::kEyesJoy;
        case roboface::Emotion::kThinking: return assets::kEyesThinking;
        case roboface::Emotion::kSurprised: return assets::kEyesSurprised;
        case roboface::Emotion::kSad: return assets::kEyesSad;
        case roboface::Emotion::kError: return assets::kEyesError;
        default: return assets::kEyesNeutral;
    }
}

inline const assets::Entry& mouthFor(roboface::Emotion emotion) {
    switch (emotion) {
        case roboface::Emotion::kCalm: return assets::kMouthCalm;
        case roboface::Emotion::kJoy: return assets::kMouthJoy;
        case roboface::Emotion::kThinking: return assets::kMouthThinking;
        case roboface::Emotion::kSurprised: return assets::kMouthSurprised;
        case roboface::Emotion::kSad: return assets::kMouthSad;
        case roboface::Emotion::kError: return assets::kMouthError;
        default: return assets::kMouthNeutral;
    }
}

//: The speaking mouth. `kClosed` reuses the neutral mouth, which is why there are four visemes and
//: not five -- a closed mouth is a closed mouth whatever produced it.
inline const assets::Entry& visemeFor(roboface::MouthFrame frame) {
    switch (frame) {
        case roboface::MouthFrame::kAjar: return assets::kMouthAjar;
        case roboface::MouthFrame::kHalf: return assets::kMouthHalf;
        case roboface::MouthFrame::kWide: return assets::kMouthWide;
        case roboface::MouthFrame::kOpen: return assets::kMouthOpen;
        default: return assets::kMouthNeutral;
    }
}

//: Where the shared features land, from the manifest the artist drew to. Named here rather than
//: taken from `FaceGeometry` because the art is one size for all five skins: the features are the
//: character and the body is the costume, so the costume was drawn around them.
inline constexpr int kEyesX = 76;
inline constexpr int kEyesY = 64;
inline constexpr int kMouthX = 100;
inline constexpr int kMouthY = 132;

//: **The sprite stores its pixels byte-swapped**, and writing raw into its buffer means matching
//: that rather than being converted into it.
//:
//: LovyanGFX keeps a 16-bit sprite in the order the panel's SPI wants, so it can be sent without
//: touching it. `drawPixel` converts on the way in; writing to `getBuffer()` skips the conversion
//: and therefore has to do it. Getting this wrong does not fail — it produces a picture, in
//: thoroughly wrong colours, which is exactly how it announced itself.
inline uint16_t toStore(uint16_t rgb) { return __builtin_bswap16(rgb); }
inline uint16_t fromStore(uint16_t stored) { return __builtin_bswap16(stored); }

//: RGB565 channel arithmetic, unpacked once per pixel rather than per channel access.
inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    const uint32_t a = alpha;
    const uint32_t ia = 255u - a;
    const uint32_t r = (((src >> 11) & 0x1F) * a + ((dst >> 11) & 0x1F) * ia) / 255u;
    const uint32_t g = (((src >> 5) & 0x3F) * a + ((dst >> 5) & 0x3F) * ia) / 255u;
    const uint32_t b = ((src & 0x1F) * a + (dst & 0x1F) * ia) / 255u;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

//: A luminance byte times a colour, which is what "shading carried by lightness" means at the point
//: of use. The three tinted bodies were drawn as neutral forms exactly so this is the whole of the
//: recolouring.
inline uint16_t tint565(uint8_t luma, uint16_t colour) {
    const uint32_t l = luma;
    const uint32_t r = (((colour >> 11) & 0x1F) * l) / 255u;
    const uint32_t g = (((colour >> 5) & 0x3F) * l) / 255u;
    const uint32_t b = ((colour & 0x1F) * l) / 255u;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

//: Draw one rectangle of a body into the canvas buffer.
//:
//: A rectangle rather than the whole thing, because that is what every frame after the first needs:
//: the features move and the body does not, so restoring the ground under a mouth is 8,640 pixels
//: rather than 76,800.
inline void drawBodyRegion(uint16_t* canvas, std::size_t skin_index, bool tinted, uint16_t tint,
                           int x, int y, int w, int h) {
    if (canvas == nullptr) return;
    const assets::Entry& body = bodyFor(skin_index);

    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > body.width ? body.width : x + w;
    const int y1 = y + h > body.height ? body.height : y + h;

    if (tinted) {
        const uint8_t* luma = assets::bytesOf(body);
        for (int row = y0; row < y1; ++row) {
            const uint8_t* src = luma + row * body.width;
            uint16_t* dst = canvas + row * body.width;
            for (int col = x0; col < x1; ++col) dst[col] = toStore(tint565(src[col], tint));
        }
        return;
    }

    const uint16_t* pixels = assets::pixelsOf(body);
    for (int row = y0; row < y1; ++row) {
        const uint16_t* src = pixels + row * body.width;
        uint16_t* dst = canvas + row * body.width;
        for (int col = x0; col < x1; ++col) dst[col] = toStore(src[col]);
    }
}

//: Blend an alpha mask onto the canvas in one colour. The eyes and mouths are white on
//: transparency by design: only the alpha carries shape, and the ink belongs to the skin.
inline void drawMask(uint16_t* canvas, const assets::Entry& mask, int x, int y, uint16_t ink,
                     uint8_t opacity = 255) {
    if (canvas == nullptr || opacity == 0) return;
    const uint8_t* alpha = assets::bytesOf(mask);

    for (int row = 0; row < mask.height; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= roboface::kScreenHeight) continue;
        const uint8_t* src = alpha + row * mask.width;
        uint16_t* dst = canvas + dy * roboface::kScreenWidth;
        for (int col = 0; col < mask.width; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= roboface::kScreenWidth) continue;
            const uint32_t a = static_cast<uint32_t>(src[col]) * opacity / 255u;
            if (a == 0) continue;
            dst[dx] = toStore(blend565(fromStore(dst[dx]), ink, static_cast<uint8_t>(a)));
        }
    }
}

//: An overlay, which keeps its own colours -- a blush is pink whatever the skin's ink is.
inline void drawOverlay(uint16_t* canvas, const assets::Entry& entry, int x, int y) {
    if (canvas == nullptr) return;
    const std::size_t pixels = static_cast<std::size_t>(entry.width) * entry.height;
    const uint16_t* colour = assets::pixelsOf(entry);
    const uint8_t* alpha = assets::bytesOf(entry) + pixels * sizeof(uint16_t);

    for (int row = 0; row < entry.height; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= roboface::kScreenHeight) continue;
        uint16_t* dst = canvas + dy * roboface::kScreenWidth;
        for (int col = 0; col < entry.width; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= roboface::kScreenWidth) continue;
            const std::size_t i = static_cast<std::size_t>(row) * entry.width + col;
            if (alpha[i] == 0) continue;
            dst[dx] = toStore(blend565(fromStore(dst[dx]), colour[i], alpha[i]));
        }
    }
}

}  // namespace art
}  // namespace app

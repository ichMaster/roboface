// The console's font, ready for M5GFX.
//
// `lgfx::U8g2font` is a constexpr wrapper around a U8g2 font array -- the same type M5GFX uses for
// its own bundled `efont` faces -- so this costs nothing at runtime and the array stays in flash.
// M5GFX decodes UTF-8 itself when drawing, so the firmware hands it the server's bytes untouched.

#pragma once

#include <M5Unified.h>

#include "app/fonts/font_cyrillic_10x20.h"
#include "pure/layout.h"

namespace app {

// -Misc-Fixed 10x20, public domain; ASCII plus U+0400..U+0523. See the font's .cpp for provenance.
inline const lgfx::U8g2font kConsoleFont{fonts::kCyrillic10x20};

// The cell this font draws in. `pure/layout.h` derives the console's column and line counts from
// these, and `test_layout` pins the arithmetic -- so a later change of font that forgets to update
// the geometry fails on a laptop rather than by overrunning the chrome band on the panel.
inline constexpr int kConsoleFontWidth = roboface::kConsoleAdvanceWidth;
inline constexpr int kConsoleFontHeight = roboface::kConsoleLineHeight;

}  // namespace app

// The screen's geometry, from features/DEVICE_UI.md §Layout.
//
// Pure, because it is arithmetic and because the rule that matters -- "never block the face";
// chrome lives in the outer 28 px band and the face keeps the central 264x184 -- is a property of
// these numbers rather than of any drawing call. Put it here and a laptop proves the bands do not
// overlap; leave it in the renderer and the only way to check is to look at a screen and judge.

#pragma once

namespace roboface {

inline constexpr int kScreenWidth = 320;
inline constexpr int kScreenHeight = 240;

// The face safe area, centred.
inline constexpr int kFaceWidth = 264;
inline constexpr int kFaceHeight = 184;
inline constexpr int kFaceLeft = (kScreenWidth - kFaceWidth) / 2;    // 28
inline constexpr int kFaceTop = (kScreenHeight - kFaceHeight) / 2;   // 28
inline constexpr int kFaceRight = kFaceLeft + kFaceWidth;            // 292
inline constexpr int kFaceBottom = kFaceTop + kFaceHeight;           // 212

// The chrome bands: everything outside the face.
inline constexpr int kBandHeight = kFaceTop;  // 28, top and bottom alike

// The microphone button: the one **control** on the screen, top-left of the upper band.
//
// **Why a button at all**, when every other control here is a gesture: a gesture has to be known
// before it can be used, and mute is the one control a person reaches for in a hurry -- mid-sentence,
// with someone else in the room. v2.4 put it on the two-finger tap, which this panel cannot detect
// (it reports a single point); moving it to the double tap worked but spent a gesture that DEVICE_UI
// had already given to affection. A target you can see costs no gesture and needs no explaining.
//
// **The hit area is wider than the glyph, deliberately.** The drawn icon is 22 px in a 28 px band,
// which is small for a fingertip (~24 px). Extending the *target* to 60x28 makes it reachable
// without making the icon shout; the whole strip is above `kFaceTop`, so it takes nothing from the
// face. Missing a mute button is worse than hitting it slightly off-centre.
inline constexpr int kMicButtonLeft = 6;
inline constexpr int kMicButtonTop = 7;
inline constexpr int kMicButtonWidth = 14;
inline constexpr int kMicButtonHeight = 15;

//: The touch target, which starts at the very corner: a finger aiming for a corner control lands
//: short as often as long, and there is nothing to the left of it to hit by mistake.
inline constexpr int kMicButtonHitWidth = 60;
inline constexpr int kMicButtonHitHeight = kBandHeight;

inline constexpr bool inMicButton(int x, int y) {
    return x >= 0 && x < kMicButtonHitWidth && y >= 0 && y < kMicButtonHitHeight;
}

// Where the status cluster sits (top-right, 12 px glyphs).
inline constexpr int kGlyphSize = 12;
inline constexpr int kStatusClusterRight = kScreenWidth - 8;
inline constexpr int kStatusClusterTop = 8;

// The console's text grid, for v0.5's serial chat console.
//
// The font is a glue asset (`app/fonts/font_cyrillic_10x20`), but the numbers it implies are
// geometry, and geometry lives here where a host test can check the arithmetic. The face area
// fits 26 columns and 9 lines of a 10x20 cell -- 260x180 of the available 264x184, so 4 px
// spare in each direction. The grid is the largest that fits, not an exact division; what
// matters is that it cannot exceed the area, which is what the host test pins.
//
// Both halves read these: the wrap in `pure/transcript.h` measures against `kConsoleColumns`, and
// the renderer steps by `kConsoleLineHeight`. One number, two users -- so they cannot drift.
inline constexpr int kConsoleAdvanceWidth = 10;
inline constexpr int kConsoleLineHeight = 20;
inline constexpr int kConsoleColumns = kFaceWidth / kConsoleAdvanceWidth;   // 26
inline constexpr int kConsoleLines = kFaceHeight / kConsoleLineHeight;      // 9

// Whether a rectangle stays clear of the face. The renderer and the chrome drawer each assert
// their own bounds with this, so "chrome never overlaps the face" is checked by arithmetic at the
// point of drawing rather than by a person squinting at a panel.
inline constexpr bool clearOfFace(int x, int y, int width, int height) {
    const int right = x + width;
    const int bottom = y + height;
    return right <= kFaceLeft || x >= kFaceRight || bottom <= kFaceTop || y >= kFaceBottom;
}

inline constexpr bool insideFace(int x, int y, int width, int height) {
    return x >= kFaceLeft && y >= kFaceTop && (x + width) <= kFaceRight &&
           (y + height) <= kFaceBottom;
}

}  // namespace roboface

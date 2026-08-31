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

//: The touch target -- **a different rectangle from the glyph, and deliberately bigger than the
//: band.**
//:
//: Where the icon is drawn and where a press counts are two questions, and this project answered
//: them with one number for a while. Measured on the board, eight presses aimed at the icon landed
//: at y = 4, 7, 10, 10, 21, 22, **29, 38** -- horizontally never further than x = 46, so width was
//: never the constraint. The band is 28 px tall and a fingertip is roughly 24, so the two misses
//: were a person aiming correctly at a target one finger high. "You have to hit the very corner"
//: was an accurate description of a 28 px ceiling.
//:
//: So the target reaches 44 px, 16 of them **inside the face area**. That is not a violation of
//: DEVICE_UI's "never block the face": nothing is *drawn* there. The glyph stays in the band, and
//: `clearOfFace` still holds for every pixel this button paints. What overlaps is where a finger
//: counts, and touch and pixels have no reason to share a rectangle.
//:
//: The cost is stated: a tap on the top-left sliver of the forehead toggles the microphone instead
//: of tickling. It is 56x16 of a 264x184 face, at the edge furthest from where anyone strokes it,
//: and the alternative is a mute button that needs to be aimed at.
inline constexpr int kMicButtonHitWidth = 84;
inline constexpr int kMicButtonHitHeight = 44;

inline constexpr bool inMicButton(int x, int y) {
    return x >= 0 && x < kMicButtonHitWidth && y >= 0 && y < kMicButtonHitHeight;
}

// The skin button: bottom-left, mirroring the microphone button above it.
//
// **Added because the gesture was not good enough**, and saying so is the point. v2.6 put face
// switching on a hold that converts after 1.2 s of silence — a gesture that has to be waited out,
// costs a listening window, and then asked for a slide along 5 px dots. On the board it was called
// stiff, and it was.
//
// So the picker gets a door: a visible target that opens it in one tap, exactly as mute did when it
// stopped being a gesture. The hold still works — it is in DEVICE_UI and costs nothing to keep —
// but nobody has to know about it.
//
// The same 84x44 as the microphone button, reaching *up* into the face area rather than down, for
// the same measured reason: a 28 px band is one fingertip tall and a corner control is aimed at
// rather than looked at.
inline constexpr int kSkinButtonWidth = 18;
inline constexpr int kSkinButtonHeight = 14;
inline constexpr int kSkinButtonLeft = 6;
inline constexpr int kSkinButtonTop = kFaceBottom + (kBandHeight - kSkinButtonHeight) / 2;

inline constexpr int kSkinButtonHitWidth = 84;
inline constexpr int kSkinButtonHitHeight = 44;
inline constexpr int kSkinButtonHitTop = kScreenHeight - kSkinButtonHitHeight;

inline constexpr bool inSkinButton(int x, int y) {
    return x >= 0 && x < kSkinButtonHitWidth && y >= kSkinButtonHitTop && y < kScreenHeight;
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

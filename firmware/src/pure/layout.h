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

// Where the status cluster sits (top-right, 12 px glyphs).
inline constexpr int kGlyphSize = 12;
inline constexpr int kStatusClusterRight = kScreenWidth - 8;
inline constexpr int kStatusClusterTop = 8;

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

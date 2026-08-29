// A recipe becomes geometry.
//
// `FaceRecipe` says *what the face is doing* -- eyes 85% open, mouth curved +0.2. It says nothing
// about where anything sits on a 320x240 panel. This file is the step between, and it is pure for a
// reason worth stating: **the renderer that follows can then be judged by looking at the screen
// rather than by reading it.** If the eyes are in the wrong place, either the numbers here are
// wrong -- and a host test says so in a millisecond -- or the drawing is, and there is nowhere else
// to look.
//
// The six layers are the ones ARCHITECTURE §The face names, in the order they composite. Nothing
// here draws, and nothing here leaves a float for the renderer to interpret: a layer arrives as
// coordinates and a colour, and the renderer's whole job is to put those on the panel.

#pragma once

#include <cstdint>

#include "pure/face.h"
#include "pure/layout.h"

namespace roboface {

//: The composite order. Background first, overlay last -- an enum rather than an array index so a
//: renderer that iterates cannot silently draw the mouth under the base.
enum class Layer : uint8_t {
    kGlow,     // the background wash; carries the mood colour
    kBase,     // the head shape
    kEyes,
    kBrows,
    kMouth,
    kOverlay,  // reserved: v2.6's skins and v2.3's lip-sync accents composite here
    kCount,
};

//: One eye, as a rounded rectangle. Openness is a *height*, not a ratio -- the renderer should not
//: be deciding what "0.85 open" means in pixels, because then two renderers would decide
//: differently and the skins of v2.6 would each look like a different character.
struct EyeShape {
    int centre_x = 0;
    int centre_y = 0;
    int half_width = 0;
    int half_height = 0;  // 0 = fully closed
};

//: One brow, as the two endpoints of a line. An angle would leave the renderer to work out where
//: the line lands; endpoints leave it nothing to get wrong.
struct BrowShape {
    int inner_x = 0;
    int inner_y = 0;
    int outer_x = 0;
    int outer_y = 0;
};

//: The mouth, as three control points of a quadratic curve: both corners and the middle. A frown
//: and a smile differ only in which side of the corners the middle sits, which is what makes the
//: mirror test in `test_layers` meaningful rather than decorative.
struct MouthShape {
    int left_x = 0;
    int left_y = 0;
    int mid_x = 0;
    int mid_y = 0;  // above the corners for a frown, below for a smile
    int right_x = 0;
    int right_y = 0;
};

//: The head, and the wash behind it.
struct BaseShape {
    int centre_x = 0;
    int centre_y = 0;
    int half_width = 0;
    int half_height = 0;
    int corner_radius = 0;
};

//: Everything the renderer needs for one frame, and nothing it has to compute.
struct LayerBank {
    BaseShape glow;
    BaseShape base;
    EyeShape left_eye;
    EyeShape right_eye;
    BrowShape left_brow;
    BrowShape right_brow;
    MouthShape mouth;
    //: 0..255, applied to every layer alike. `dim` is a property of the *face*, not of a layer:
    //: dimming each layer separately would let a tired face come apart into differently-lit pieces.
    uint8_t brightness = 255;
};

//: Where the face sits and how large its features are. Separated from the bank so a skin can move
//: the geometry without touching the recipe-to-shape arithmetic -- v2.6 needs exactly that.
struct FaceGeometry {
    int centre_x = kFaceLeft + kFaceWidth / 2;
    int centre_y = kFaceTop + kFaceHeight / 2;
    int half_width = kFaceWidth / 2;
    int half_height = kFaceHeight / 2;

    int eye_spacing = 62;      // centre to centre, halved either side of the midline
    int eye_half_width = 26;
    int eye_open_height = 30;  // the half-height at `eye_openness == 1`
    int eye_offset_y = -22;    // above the face's centre

    int brow_offset_y = -58;   // above the face's centre
    int brow_half_width = 26;
    int brow_travel = 12;      // how far an end moves at full angle

    int mouth_offset_y = 42;   // below the face's centre
    int mouth_half_width = 40;
    int mouth_travel = 22;     // how far the middle moves at full curve

    int max_tilt_px = 14;      // horizontal shift of the features at `tilt == ±1`
};

namespace detail {

//: Rounding that behaves the same either side of zero. `static_cast<int>` truncates toward zero, so
//: a smile of +0.5 and a frown of -0.5 would land a pixel apart and the mirror test would fail for
//: a reason that has nothing to do with faces.
inline constexpr int roundToInt(float value) {
    return value >= 0.0f ? static_cast<int>(value + 0.5f) : -static_cast<int>(-value + 0.5f);
}

inline constexpr float clampUnit(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

}  // namespace detail

// Turn a recipe into shapes. Total over every recipe the table can produce: the clamps below are
// the guarantee, not a defensive habit -- a recipe interpolated mid-crossfade can briefly sit
// slightly outside 0..1, and a face that inverts for one frame is very visible.
inline constexpr LayerBank layout(const FaceRecipe& recipe, const FaceGeometry& geometry = {}) {
    LayerBank bank;

    const float openness = detail::clampUnit(recipe.eye_openness, 0.0f, 1.0f);
    const float curve = detail::clampUnit(recipe.mouth_curve, -1.0f, 1.0f);
    const float brow = detail::clampUnit(recipe.brow_angle, -1.0f, 1.0f);
    const float tilt = detail::clampUnit(recipe.tilt, -1.0f, 1.0f);
    const float dim = detail::clampUnit(recipe.dim, 0.0f, 1.0f);

    // Tilt shifts the features rather than rotating them. Rotation on this panel means resampling
    // every layer, which is exactly the frame budget RF-058 has to defend; a shift reads as a head
    // turn at this size and costs nothing.
    const int shift = detail::roundToInt(tilt * static_cast<float>(geometry.max_tilt_px));
    const int cx = geometry.centre_x + shift;
    const int cy = geometry.centre_y;

    bank.glow = BaseShape{geometry.centre_x, cy, geometry.half_width, geometry.half_height,
                          geometry.half_height / 2};
    bank.base = BaseShape{cx, cy, geometry.half_width - 10, geometry.half_height - 10,
                          (geometry.half_height - 10) / 2};

    const int eye_y = cy + geometry.eye_offset_y;
    const int eye_half = geometry.eye_spacing / 2;
    const int eye_height = detail::roundToInt(openness * static_cast<float>(geometry.eye_open_height));
    bank.left_eye = EyeShape{cx - eye_half, eye_y, geometry.eye_half_width, eye_height};
    bank.right_eye = EyeShape{cx + eye_half, eye_y, geometry.eye_half_width, eye_height};

    // Positive angle drops the inner ends (cross); negative raises them (worried). The outer ends
    // move the opposite way by half, which is what makes the brow read as tilted rather than slid.
    const int brow_y = cy + geometry.brow_offset_y;
    const int inner_drop = detail::roundToInt(brow * static_cast<float>(geometry.brow_travel));
    const int outer_drop = -inner_drop / 2;
    bank.left_brow = BrowShape{cx - eye_half + geometry.brow_half_width, brow_y + inner_drop,
                               cx - eye_half - geometry.brow_half_width, brow_y + outer_drop};
    bank.right_brow = BrowShape{cx + eye_half - geometry.brow_half_width, brow_y + inner_drop,
                                cx + eye_half + geometry.brow_half_width, brow_y + outer_drop};

    // The middle of the mouth moves *down* for a frown and *up* for a smile, in screen coordinates
    // where y grows downward -- hence the negation. Getting this backwards produces a face that is
    // cheerful when it should be sad, which no test of magnitudes alone would catch.
    const int mouth_y = cy + geometry.mouth_offset_y;
    const int mid_offset = detail::roundToInt(-curve * static_cast<float>(geometry.mouth_travel));
    bank.mouth = MouthShape{cx - geometry.mouth_half_width, mouth_y,
                            cx,                             mouth_y + mid_offset,
                            cx + geometry.mouth_half_width, mouth_y};

    bank.brightness = static_cast<uint8_t>(detail::roundToInt((1.0f - dim) * 255.0f));
    return bank;
}

}  // namespace roboface

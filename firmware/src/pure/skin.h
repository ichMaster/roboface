// What a skin *is*, as data.
//
// `CLAUDE.md`: *"Skins are asset swaps. All five faces render the same `EmotionFrame`. If a skin
// needs renderer logic, the design is wrong."* This header is what makes that enforceable rather
// than aspirational — a skin is a `Skin`, and a `Skin` is a struct. There is nowhere to put logic.
//
// **The schema is not invented. It is `face-prototype.html`'s own argument lists, made data.**
// CLAUDE.md is explicit that the prototype is the reference and that a second structure must not be
// invented beside it. In the prototype every skin calls the same two functions:
//
//     sEyes(emotion, left_x, right_x, y, ink, highlight, gaze_px)
//     sMouth(emotion, centre_x, centre_y, ink)
//
// and then adds a silhouette and one distinguishing behaviour. So a skin is: **anchors, colours, a
// silhouette, and an element.** Those four things and nothing else.
//
// **Totality over the emotion enum is the property this file exists to guarantee.** A skin that
// answers for six emotions and not the seventh is a face that vanishes when someone is sad, and it
// is precisely the kind of gap that ships: the missing one is rare by construction, so it is the
// one nobody exercises. Every table here is indexed by `Emotion` and sized by `kCount`, which makes
// a missing entry a compile error rather than a blank screen.
//
// Pure: header-only, `namespace roboface`, no <M5Unified.h>, no allocation.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/face.h"
#include "pure/layers.h"
#include "pure/layout.h"

namespace roboface {

//: How many skins exist. The carousel's dot count and the manifest table's size are the same
//: number, and DEVICE_UI's strip is drawn from it.
inline constexpr std::size_t kSkinCount = 5;

//: The one thing each spirit does that the others do not.
//:
//: **A closed enum, deliberately, and not a script or a callback.** A skin that needed a sixth
//: entry would be telling us the schema is too narrow — and an enum is where that shows up
//: immediately, at the moment someone tries to add it, rather than three phases later when a
//: renderer has quietly grown four special cases. The prototype has exactly these four, and the
//: fifth face has none.
enum class SkinElement : uint8_t {
    kNone,                    // stackchan: the face is the whole of it
    kBlushAndTear,            // ghost: pink cheeks when content, a tear when sad
    kPaletteFollowsEmotion,   // flame: the fire's colour is the mood
    kGlowFollowsEmotion,      // jelly: the bell's glow is the mood
    kWeatherFollowsEmotion,   // cloud: rain, sun, storm
    kCount,
};

inline constexpr const char* toString(SkinElement element) {
    switch (element) {
        case SkinElement::kNone: return "none";
        case SkinElement::kBlushAndTear: return "blush_and_tear";
        case SkinElement::kPaletteFollowsEmotion: return "palette";
        case SkinElement::kGlowFollowsEmotion: return "glow";
        case SkinElement::kWeatherFollowsEmotion: return "weather";
        case SkinElement::kCount: break;
    }
    return "none";
}

//: A colour per emotion. **Sized by `kCount`, which is the whole point**: adding an emotion to the
//: enum breaks every skin at once, loudly, instead of leaving the newest one undrawn in exactly the
//: five places nobody looks.
struct EmotionPalette {
    uint16_t colour[static_cast<std::size_t>(Emotion::kCount)] = {};

    constexpr uint16_t at(Emotion emotion) const {
        const auto index = static_cast<std::size_t>(emotion);
        return index < static_cast<std::size_t>(Emotion::kCount) ? colour[index] : colour[0];
    }
};

//: The silhouette behind the features, as the small set of shapes the renderer can draw. A skin
//: describes its body; it does not supply one.
enum class SkinBody : uint8_t {
    kNone,      // stackchan: a bare face on the background
    kGhost,     // a rounded dome with a scalloped hem
    kFlame,     // a teardrop, wider at the base
    kBell,      // the jellyfish's bell, with tendrils below
    kCloud,     // overlapping lobes
    kCount,
};

//: Everything a face is.
//:
//: Flat and copyable on purpose: a skin is loaded once, held in PSRAM, and read every frame. A
//: structure with pointers into a pack would make every read a question about whether the pack is
//: still there — and RF-083's whole subject is packs that are not.
struct Skin {
    const char* name = "stackchan";

    //: Where the features sit. `FaceGeometry` is already the renderer's language, so a skin
    //: supplies one rather than a parallel set of numbers that would have to be kept in step.
    FaceGeometry geometry;

    SkinBody body = SkinBody::kNone;
    SkinElement element = SkinElement::kNone;

    //: The features' colour, and the highlight in an eye. `sEyes(…, ink, hi, …)` in the prototype.
    uint16_t ink = 0xFFFF;
    uint16_t highlight = 0xFFFF;

    //: The silhouette's own colour, and what is behind it.
    uint16_t body_colour = 0xFFFF;
    uint16_t background = 0x0000;

    //: How far the features travel at `gaze == ±1`. Per-skin because a small face and a large one
    //: cannot move the same number of pixels and read the same.
    int gaze_travel_px = 8;

    //: What the element does, per emotion. Unused when `element` is `kNone`, and **still total** --
    //: a table that is only sometimes filled in is a table someone will read when it is not.
    EmotionPalette element_palette;
};

//: Why a manifest was refused. **A reason, not a bool** -- RF-083's fallback prints this, and a
//: boolean would make "the file was truncated" and "the eyes are off the screen" the same event.
enum class SkinFault : uint8_t {
    kNone,
    kNoName,
    kEyesOutsideFace,
    kMouthOutsideFace,
    kEyesOverlap,
    kUnknownBody,
    kUnknownElement,
    kElementPaletteMissing,
    kCount,
};

inline constexpr const char* toString(SkinFault fault) {
    switch (fault) {
        case SkinFault::kNone: return "ok";
        case SkinFault::kNoName: return "skin has no name";
        case SkinFault::kEyesOutsideFace: return "eyes fall outside the face safe area";
        case SkinFault::kMouthOutsideFace: return "mouth falls outside the face safe area";
        case SkinFault::kEyesOverlap: return "the eyes overlap each other";
        case SkinFault::kUnknownBody: return "unknown body shape";
        case SkinFault::kUnknownElement: return "unknown element";
        case SkinFault::kElementPaletteMissing: return "element has no palette for some emotion";
        case SkinFault::kCount: break;
    }
    return "ok";
}

//: Whether a manifest can be drawn, and if not, which field is wrong.
//:
//: **The safe-area checks are the ones that matter**, and they are checked here rather than at the
//: point of drawing for the reason `layout.h` gives about chrome: a rule expressed as arithmetic can
//: be proven on a laptop, and a rule expressed as a drawing call can only be checked by looking at a
//: screen and judging. A skin whose eyes are 20 px too high does not crash -- it draws over the
//: battery indicator, and someone notices three weeks later.
inline constexpr SkinFault validate(const Skin& skin) {
    if (skin.name == nullptr || skin.name[0] == '\0') return SkinFault::kNoName;
    if (skin.body >= SkinBody::kCount) return SkinFault::kUnknownBody;
    if (skin.element >= SkinElement::kCount) return SkinFault::kUnknownElement;

    const FaceGeometry& g = skin.geometry;
    const int eye_half = g.eye_spacing / 2;
    if (eye_half <= g.eye_half_width) return SkinFault::kEyesOverlap;

    const int eye_top = g.centre_y + g.eye_offset_y - g.eye_open_height;
    const int eye_bottom = g.centre_y + g.eye_offset_y + g.eye_open_height;
    const int eye_left = g.centre_x - eye_half - g.eye_half_width - skin.gaze_travel_px;
    const int eye_right = g.centre_x + eye_half + g.eye_half_width + skin.gaze_travel_px;
    if (eye_top < kFaceTop || eye_bottom > kFaceBottom || eye_left < kFaceLeft ||
        eye_right > kFaceRight) {
        return SkinFault::kEyesOutsideFace;
    }

    const int mouth_top = g.centre_y + g.mouth_offset_y - g.mouth_open_travel;
    const int mouth_bottom = g.centre_y + g.mouth_offset_y + g.mouth_open_travel;
    const int mouth_left = g.centre_x - g.mouth_half_width - skin.gaze_travel_px;
    const int mouth_right = g.centre_x + g.mouth_half_width + skin.gaze_travel_px;
    if (mouth_top < kFaceTop || mouth_bottom > kFaceBottom || mouth_left < kFaceLeft ||
        mouth_right > kFaceRight) {
        return SkinFault::kMouthOutsideFace;
    }

    // An element that follows the emotion needs a colour for **every** emotion. Zero is black, and
    // a black flame is a skin that vanishes for one mood -- the exact failure this schema is shaped
    // to prevent, so it is checked rather than assumed.
    if (skin.element == SkinElement::kPaletteFollowsEmotion ||
        skin.element == SkinElement::kGlowFollowsEmotion ||
        skin.element == SkinElement::kWeatherFollowsEmotion) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(Emotion::kCount); ++i) {
            if (skin.element_palette.colour[i] == 0) return SkinFault::kElementPaletteMissing;
        }
    }

    return SkinFault::kNone;
}

}  // namespace roboface

// The five faces, as manifests.
//
// **Stackchan is in here beside the spirits, and that is the whole design.** It could have stayed
// as constants in the renderer -- it worked, and nothing was asking for it to move. But a renderer
// that knows one face specially is a renderer every future skin has to negotiate with, and the
// phase DoD is *"adding a skin requires no renderer code change"*. That claim is only testable if
// the face whose correct appearance is already known is expressed the same way as the ones that are
// not: if the procedural face cannot be a manifest, the schema is wrong, and it is far better to
// learn that from a face we can compare against a screenshot than from the fourth spirit.
//
// The coordinates are `face-prototype.html`'s, not an interpretation of them. CLAUDE.md: *"Port that
// structure to M5GFX sprites; don't invent a second one."*
//
// Pure: header-only, `namespace roboface`, no allocation, no <M5Unified.h>.

#pragma once

#include <cstddef>

#include "pure/skin.h"

namespace roboface {

//: RGB565. Written as hex because that is what the panel takes and what the prototype's `#rrggbb`
//: converts to -- a second representation would be a second thing to get wrong.
inline constexpr uint16_t kInkCyan = 0x5DFF;    // the soft cyan-white of the procedural features
inline constexpr uint16_t kNightBlue = 0x0104;  // the wash behind a head
inline constexpr uint16_t kGhostWhite = 0xF7BE;
inline constexpr uint16_t kGhostInk = 0x10A5;
inline constexpr uint16_t kNightSky = 0x0862;

//: A palette filled from one colour, for a skin whose element does not follow the emotion but whose
//: table must still be total. Spelled once rather than at five call sites.
inline constexpr EmotionPalette uniform(uint16_t colour) {
    EmotionPalette palette;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Emotion::kCount); ++i) {
        palette.colour[i] = colour;
    }
    return palette;
}

//: The palette a mood-following element uses. **Ordered as `Emotion` declares** -- neutral, calm,
//: joy, thinking, surprised, sad, error -- and every entry non-zero, which `validate()` enforces.
inline constexpr EmotionPalette moodPalette(uint16_t neutral, uint16_t calm, uint16_t joy,
                                            uint16_t thinking, uint16_t surprised, uint16_t sad,
                                            uint16_t error) {
    EmotionPalette palette;
    palette.colour[static_cast<std::size_t>(Emotion::kNeutral)] = neutral;
    palette.colour[static_cast<std::size_t>(Emotion::kCalm)] = calm;
    palette.colour[static_cast<std::size_t>(Emotion::kJoy)] = joy;
    palette.colour[static_cast<std::size_t>(Emotion::kThinking)] = thinking;
    palette.colour[static_cast<std::size_t>(Emotion::kSurprised)] = surprised;
    palette.colour[static_cast<std::size_t>(Emotion::kSad)] = sad;
    palette.colour[static_cast<std::size_t>(Emotion::kError)] = error;
    return palette;
}

//: **The face the device has worn since v2.1**, now expressed as data. Every number here was a
//: constant in `procedural_renderer.cpp` or a default in `FaceGeometry`, and a host test asserts
//: they are still the same values -- the regression that would otherwise change the face quietly.
inline constexpr Skin stackchan() {
    Skin skin;
    skin.name = "stackchan";
    skin.body = SkinBody::kNone;
    skin.element = SkinElement::kNone;
    skin.ink = kInkCyan;
    skin.highlight = 0xFFFF;
    skin.body_colour = kNightBlue;  // the glow behind the head
    skin.background = 0x0000;       // the panel's own dark is the room
    skin.gaze_travel_px = 14;       // `FaceGeometry::max_tilt_px`
    skin.element_palette = uniform(kInkCyan);
    return skin;
}

//: **Ghost.** Anchors from the prototype: `sEyes(e, 122, 198, 118, …)`, `sMouth(e, 160, 158, …)`,
//: `gx = gaze.x * 8`. Expressed as offsets from the face centre (160, 120), which is what
//: `FaceGeometry` speaks -- the same two numbers, in the renderer's units rather than the SVG's.
inline constexpr Skin ghost() {
    Skin skin;
    skin.name = "ghost";
    skin.body = SkinBody::kGhost;
    skin.element = SkinElement::kBlushAndTear;
    skin.ink = kGhostInk;         // #151528
    skin.highlight = 0xFFFF;      // #fff
    skin.body_colour = kGhostWhite;  // #f2f4ff
    skin.background = 0x0863;     // #0a0d1c, the night sky
    skin.gaze_travel_px = 8;
    skin.geometry.eye_spacing = 76;    // 198 - 122
    skin.geometry.eye_offset_y = -2;   // 118 - 120
    skin.geometry.eye_half_width = 20;
    skin.geometry.eye_open_height = 22;
    skin.geometry.mouth_offset_y = 38;  // 158 - 120
    skin.geometry.mouth_half_width = 30;
    //: The blush, which is a fixed pink rather than a mood colour -- so the table is uniform and
    //: still total. `validate()` does not require a palette for this element; filling it anyway is
    //: what stops a later reader finding a half-empty array and wondering.
    skin.element_palette = uniform(0xFD98);  // #ffb3c2
    return skin;
}

//: **Flame.** `sEyes(e, 128, 192, 138, …)`, `sMouth(e, 160, 178, …)`. Its element is the fire's
//: palette, which in the prototype is a four-stop gradient chosen per emotion; here it is the
//: dominant stop, because a device that redraws eighteen times a second cannot afford four.
inline constexpr Skin flame() {
    Skin skin;
    skin.name = "flame";
    skin.body = SkinBody::kFlame;
    skin.element = SkinElement::kPaletteFollowsEmotion;
    skin.ink = 0x3880;            // #3a1206
    skin.highlight = 0xFFDF;
    skin.body_colour = 0xFAC3;    // the resting orange
    skin.background = 0x0863;
    skin.gaze_travel_px = 8;
    skin.geometry.eye_spacing = 64;    // 192 - 128
    skin.geometry.eye_offset_y = 18;   // 138 - 120
    skin.geometry.eye_half_width = 20;
    skin.geometry.eye_open_height = 20;
    skin.geometry.mouth_offset_y = 58;  // 178 - 120
    skin.geometry.mouth_half_width = 28;
    //: Sad is blue and error is red — the prototype's own choices, and the reason this element
    //: exists at all: the flame *is* the mood, so its colour is not decoration.
    skin.element_palette = moodPalette(0xFAC3, 0xFAC3, 0xFC43, 0xFAC3, 0xFD83, 0x2AFF, 0xF9C6);
    return skin;
}

//: **Jellyfish.** `sEyes(e, 128, 192, 120, …)`, `sMouth(e, 160, 142, …)`. The bell's glow is the
//: mood; the tendrils take the same colour.
inline constexpr Skin jelly() {
    Skin skin;
    skin.name = "jelly";
    skin.body = SkinBody::kBell;
    skin.element = SkinElement::kGlowFollowsEmotion;
    skin.ink = 0x2867;            // #2a0f3a
    skin.highlight = 0xFFFF;
    skin.body_colour = 0xC47F;    // #c58fff at rest
    skin.background = 0x0863;
    skin.gaze_travel_px = 8;
    skin.geometry.eye_spacing = 64;
    skin.geometry.eye_offset_y = 0;    // 120 - 120
    skin.geometry.eye_half_width = 20;
    skin.geometry.eye_open_height = 22;
    skin.geometry.mouth_offset_y = 22;  // 142 - 120
    skin.geometry.mouth_half_width = 26;
    skin.element_palette = moodPalette(0xC47F, 0xC47F, 0xFC7A, 0xC47F, 0xC47F, 0x6CFF, 0xFA6B);
    return skin;
}

//: **Cloud.** `sEyes(e, 130, 190, 132, …)`, `sMouth(e, 160, 166, …)`. Its element is the weather:
//: the body itself darkens for error, pales for sadness, and gains a sun for joy.
inline constexpr Skin cloud() {
    Skin skin;
    skin.name = "cloud";
    skin.body = SkinBody::kCloud;
    skin.element = SkinElement::kWeatherFollowsEmotion;
    skin.ink = 0x3A0B;            // #39405c
    skin.highlight = 0xFFFF;
    skin.body_colour = 0xEF9F;    // #eef2f8
    skin.background = 0x2D7B;     // a daylight sky, not the night the others sit in
    skin.gaze_travel_px = 8;
    skin.geometry.eye_spacing = 60;    // 190 - 130
    skin.geometry.eye_offset_y = 12;   // 132 - 120
    skin.geometry.eye_half_width = 18;
    skin.geometry.eye_open_height = 20;
    skin.geometry.mouth_offset_y = 46;  // 166 - 120
    skin.geometry.mouth_half_width = 26;
    //: The **body's** colour per mood, which is what "weather" means for this skin -- overcast grey
    //: for error, a washed pale for sadness, bright white otherwise.
    skin.element_palette = moodPalette(0xEF9F, 0xEF9F, 0xEF9F, 0xEF9F, 0xEF9F, 0xC6BD, 0x5B2F);
    return skin;
}

//: All five, in carousel order. **One array, and `kSkinCount` sizes it** -- the dot strip, the
//: `face_set` vocabulary and the fallback all read this, so a sixth skin is one entry rather than
//: four edits that can disagree.
inline constexpr Skin skinAt(std::size_t index) {
    switch (index) {
        case 1: return ghost();
        case 2: return flame();
        case 3: return jelly();
        case 4: return cloud();
        default: return stackchan();
    }
}

//: The index a name refers to, or `kSkinCount` when nothing does. **Not a default of zero** -- an
//: unknown `face_set` must be refusable, and silently wearing stackchan would make a typo in a
//: server config indistinguishable from a deliberate choice.
inline std::size_t skinIndexFor(const char* name) {
    if (name == nullptr) return kSkinCount;
    for (std::size_t i = 0; i < kSkinCount; ++i) {
        const char* candidate = skinAt(i).name;
        std::size_t j = 0;
        while (candidate[j] != '\0' && name[j] != '\0' && candidate[j] == name[j]) ++j;
        if (candidate[j] == '\0' && name[j] == '\0') return i;
    }
    return kSkinCount;
}

//: What loading a skin produced.
struct SkinLoad {
    Skin skin;
    SkinFault fault = SkinFault::kNone;
    //: Whether the procedural face was substituted. **Distinct from `fault != kNone`** so a caller
    //: cannot conflate "the pack was fine" with "the pack was broken and we coped": the second one
    //: needs a line in a log, and telling them apart is exactly what v2.4's proximity sensor could
    //: not do.
    bool fell_back = false;
};

//: Take a manifest, or fall back to the procedural face.
//:
//: **A face is never absent, and that is the definition of the feature rather than an error path.**
//: A device that cannot draw a face has no way to tell anyone anything -- not that a pack is
//: broken, not that the WiFi is down, not that it is listening. So there is no failure mode here
//: in which nothing is worn; there is only "what was asked for" and "the one that always works".
//:
//: The fault is carried out rather than swallowed. v2.4's lesson, literally: `begin()` must not
//: report success for something it did not load, and asking whether a thing exists is a different
//: question from asking whether it validated.
inline SkinLoad loadSkin(const Skin& candidate) {
    SkinLoad loaded;
    loaded.fault = validate(candidate);
    if (loaded.fault == SkinFault::kNone) {
        loaded.skin = candidate;
        return loaded;
    }
    loaded.skin = stackchan();
    loaded.fell_back = true;
    return loaded;
}

//: The same, by index -- an index nothing answers to is as much a missing pack as a corrupt file.
inline SkinLoad loadSkinAt(std::size_t index) {
    if (index >= kSkinCount) {
        SkinLoad loaded;
        loaded.skin = stackchan();
        loaded.fault = SkinFault::kNoName;
        loaded.fell_back = true;
        return loaded;
    }
    return loadSkin(skinAt(index));
}

}  // namespace roboface

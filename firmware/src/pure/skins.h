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

}  // namespace roboface

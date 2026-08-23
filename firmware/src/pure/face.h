// The state -> face recipe table.
//
// A `FaceRecipe` is the small set of numbers a face is drawn from. It carries **no colours, no
// pixels and no drawing** -- a renderer decides how to express openness and curvature, which is
// exactly what lets v2's five skins (Stack-chan, ghost, flame, jelly, cloud) share one expression
// grammar (ARCHITECTURE §The face → Skins). If a skin ever needs renderer logic, the design is
// wrong; this header is where that stays true.
//
// This is the v0 ancestor of v2's emotion recipe table. v2.2 changes the *source* of a recipe --
// an `EmotionFrame` from the server instead of the device's own state -- without changing the idea
// of one. Getting the shape right now is the whole point of building a stub renderer at all.
//
// Pure: header-only, `namespace roboface`, no M5GFX, host-tested.

#pragma once

#include "pure/state.h"

namespace roboface {

// Every field is normalised, so a renderer scales them to whatever geometry it has and a skin can
// reinterpret them without renegotiating units.
struct FaceRecipe {
    float eye_openness = 1.0f;  // 0 = closed, 1 = wide
    float mouth_curve = 0.0f;   // -1 = frown, 0 = flat, +1 = smile
    float brow_angle = 0.0f;    // -1 = worried (inner ends up), +1 = cross (inner ends down)
    float tilt = 0.0f;          // -1 = left, +1 = right, as a fraction of the renderer's max tilt
    float dim = 0.0f;           // 0 = full brightness, 1 = black
};

// The recipe for a device state. **Total over the enum** -- every state has a face, including the
// ones this version never enters. A table that covered only what v0.4 can reach would fail the
// moment v1 drives `kListening`, and it would fail on the screen, which is the worst place to
// discover a missing case.
inline constexpr FaceRecipe recipeFor(DeviceState state) {
    switch (state) {
        // Eyes closed, opening -- DEVICE_UI §Screens. The face a person sees for a fraction of a
        // second before anything else happens.
        case DeviceState::kBoot:
            return FaceRecipe{0.05f, 0.0f, 0.0f, 0.0f, 0.25f};

        // `calm`, gaze drifting. Slightly narrowed and tilted: looking about, not at you.
        case DeviceState::kWifiConnecting:
            return FaceRecipe{0.70f, 0.05f, -0.10f, 0.25f, 0.10f};

        // The resting face. Open, faintly pleased, level -- everything else is a departure from
        // this, which is what makes the others readable as departures.
        case DeviceState::kIdle:
            return FaceRecipe{0.85f, 0.20f, 0.0f, 0.0f, 0.0f};

        // v1 drives this. Wide and attentive, head cocked: the difference between "listening" and
        // "idle" has to be visible across a room, because it is the difference between the device
        // hearing you and not.
        case DeviceState::kListening:
            return FaceRecipe{1.0f, 0.05f, -0.25f, -0.35f, 0.0f};

        // Narrowed, brows drawn, tilted the other way from listening. Concentration reads as less
        // eye and more brow.
        case DeviceState::kThinking:
            return FaceRecipe{0.45f, -0.05f, 0.45f, 0.30f, 0.0f};

        // Talking: open, clearly smiling, level. From v1 the mouth is driven by the lip-sync
        // envelope; until then a fixed open smile is the honest stub.
        case DeviceState::kReplying:
            return FaceRecipe{0.80f, 0.70f, -0.10f, 0.0f, 0.0f};

        // `sad`, dimmed to ~60 % -- DEVICE_UI §Screens says so explicitly, and the dimming is what
        // makes offline read as *reduced* rather than merely unhappy.
        case DeviceState::kOffline:
            return FaceRecipe{0.35f, -0.55f, -0.60f, -0.15f, 0.40f};

        // The error face. **Not dimmed**: a fault is the one thing that must not recede, and the
        // contrast with `offline` is deliberate -- one is the device being unable, the other is
        // something being wrong.
        case DeviceState::kError:
            return FaceRecipe{0.95f, -0.75f, 0.85f, 0.0f, 0.0f};
    }
    return FaceRecipe{};
}

// How different two recipes are, as the largest difference across any single field.
//
// A single number, deliberately: "visibly distinct" is the DoD's bar, and two faces that differ by
// a little in every field are *not* reliably distinguishable across a room, while two that differ a
// lot in one field are. Taking the maximum rather than a sum is what encodes that.
inline constexpr float recipeDistance(const FaceRecipe& a, const FaceRecipe& b) {
    const float differences[] = {
        a.eye_openness - b.eye_openness, a.mouth_curve - b.mouth_curve, a.brow_angle - b.brow_angle,
        a.tilt - b.tilt,                 a.dim - b.dim,
    };
    float largest = 0.0f;
    for (const float d : differences) {
        const float magnitude = d < 0 ? -d : d;
        if (magnitude > largest) largest = magnitude;
    }
    return largest;
}

// The threshold at which two faces are far enough apart to be told apart at arm's length on a
// 320x240 panel. Chosen so the table above passes with room to spare rather than exactly -- a bar
// that only just clears is one that a small future tweak silently breaks.
inline constexpr float kDistinctEnough = 0.25f;

inline constexpr bool areDistinct(const FaceRecipe& a, const FaceRecipe& b) {
    return recipeDistance(a, b) >= kDistinctEnough;
}

}  // namespace roboface

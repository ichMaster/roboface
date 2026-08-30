// The state -> face recipe table.
//
// A `FaceRecipe` is the small set of numbers a face is drawn from. It carries **no colours, no
// pixels and no drawing** -- a renderer decides how to express openness and curvature, which is
// exactly what lets v2's five skins (Stack-chan, ghost, flame, jelly, cloud) share one expression
// grammar (ARCHITECTURE §The face → Skins). If a skin ever needs renderer logic, the design is
// wrong; this header is where that stays true.
//
// **From v2.2 the source of a recipe is the server**, as an `EmotionFrame`, and the shape v0 built
// against a stub survived that change unaltered -- which is what building the stub was for.
//
// Two tables now, and the split is the authority rule rather than an accident. `recipeFor(Emotion)`
// is the server's vocabulary: what the character feels. `recipeFor(DeviceState)` is what is left
// after v2.2 took the turn states away -- `boot`, `wifi_connecting`, `offline`, a local fault --
// and those stay local for a reason that is not negotiable: they are facts about the link, and in
// the `offline` case they are by definition facts about the server being unreachable. A device that
// waited for the server to tell it how to look while disconnected would have no face at all.
//
// Pure: header-only, `namespace roboface`, no M5GFX, host-tested.

#pragma once

#include <cstdint>
#include <cstring>

#include "pure/state.h"

namespace roboface {

//: The face vocabulary the server sends (ARCHITECTURE §EmotionFrame). Seven values, and the count
//: is a contract rather than a convenience: `recipeFor` below must be **total** over this enum,
//: because a value with no recipe does not fail anywhere -- it draws a blank face on a desk, with
//: no stack trace and nothing in a log.
enum class Emotion : uint8_t {
    kNeutral,
    kCalm,
    kJoy,
    kThinking,
    kSurprised,
    kSad,
    kError,
    kCount,
};

//: What an omitted `intensity` means, matching the server's own default. The midpoint, because it
//: is the only value wrong by the same amount in both directions.
inline constexpr float kDefaultIntensity = 0.5f;

//: What an omitted `ttl_ms` means, matching the server's `DEFAULT_TTL_MS`. Both halves apply it,
//: which is what lets the server omit the field on every frame that does not change it.
inline constexpr uint32_t kDefaultTtlMs = 8000;

inline constexpr const char* toString(Emotion emotion) {
    switch (emotion) {
        case Emotion::kNeutral: return "neutral";
        case Emotion::kCalm: return "calm";
        case Emotion::kJoy: return "joy";
        case Emotion::kThinking: return "thinking";
        case Emotion::kSurprised: return "surprised";
        case Emotion::kSad: return "sad";
        case Emotion::kError: return "error";
        case Emotion::kCount: break;
    }
    return "neutral";
}

//: A wire string -> an emotion. **Unknown becomes `neutral`, never a failure.**
//:
//: The server applies this same rule before sending, and doing it again here is not redundancy: the
//: two tiers are separately releasable, so neither may assume the other has already sanitised what
//: it sends. It is also the cheaper half of the guarantee -- the alternative is a blank face.
inline Emotion emotionFrom(const char* name) {
    if (name == nullptr) return Emotion::kNeutral;
    for (uint8_t index = 0; index < static_cast<uint8_t>(Emotion::kCount); ++index) {
        const auto candidate = static_cast<Emotion>(index);
        if (std::strcmp(name, toString(candidate)) == 0) return candidate;
    }
    return Emotion::kNeutral;
}

//: The whole face channel, as the device holds it (ARCHITECTURE §EmotionFrame). Every field
//: carries the documented default, so a frame that omitted a field and one that never mentioned it
//: are the same object -- which is what lets the server omit every optional field at its default.
//:
//: `gaze` is present as data and unused as behaviour until v2.4 gives the device somewhere to put
//: it. Parsed now rather than later because the *contract* is fixed now, and a field the device
//: silently drops is a field the server cannot tell is being ignored.
struct EmotionFrame {
    Emotion emotion = Emotion::kNeutral;
    float intensity = kDefaultIntensity;
    bool has_gaze = false;
    float gaze_x = 0.0f;
    float gaze_y = 0.0f;
    //: **A permission, not a duration.** It may allow the lip-sync to run; what decides when the
    //: mouth stops is the device's own playback state. The server is seconds ahead of the speaker
    //: because the device is still draining audio the server finished sending, and taking this
    //: field literally is precisely what froze the mouth mid-reply before `v2.1.2`.
    bool speaking = false;
    //: With no new frame for this long, the face relaxes to `neutral`.
    uint32_t ttl_ms = kDefaultTtlMs;
};

inline constexpr float clampUnit(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

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
// The recipe for an emotion -- **the server's table**, and from v2.2 the one that runs during a
// turn. Total over the enum, for the reason stated at `Emotion`: a missing case is a blank face
// rather than an error.
//
// These are departures from `neutral`, and they are meant to be read as a set rather than one at a
// time. What separates them across a room is not detail but *direction*: the brows, and whether the
// eyes are opening or narrowing. Two emotions that differ only in mouth curvature are two emotions
// nobody can tell apart at 320x240 from a metre away.
inline constexpr FaceRecipe recipeFor(Emotion emotion) {
    switch (emotion) {
        // The resting face. Everything below is a departure from this, which is what makes the
        // others readable *as* departures.
        case Emotion::kNeutral:
            return FaceRecipe{0.85f, 0.20f, 0.0f, 0.0f, 0.0f};

        // Attentive and settled. Slightly wider than neutral and faintly tilted: listening to you,
        // rather than merely awake.
        case Emotion::kCalm:
            return FaceRecipe{0.95f, 0.15f, -0.15f, -0.20f, 0.0f};

        // Wide, smiling, brows lifted. The one unambiguously warm face, and the only one where
        // every field moves in the same direction.
        case Emotion::kJoy:
            return FaceRecipe{1.0f, 0.85f, -0.30f, 0.10f, 0.0f};

        // Concentration reads as *less eye and more brow*, which is why this narrows rather than
        // widens -- the opposite of surprise, and the reason the two are never confused.
        case Emotion::kThinking:
            return FaceRecipe{0.45f, -0.05f, 0.45f, 0.30f, 0.0f};

        // Everything open at once: eyes at full, brows up hard, mouth slightly open. The face that
        // has to be legible in a single frame, because it is usually gone by the next one.
        case Emotion::kSurprised:
            return FaceRecipe{1.0f, 0.10f, -0.75f, 0.0f, 0.0f};

        // Down, and **dimmed**. Sadness that is only a frown reads as mild distaste; the dimming is
        // what makes it read as withdrawn.
        case Emotion::kSad:
            return FaceRecipe{0.40f, -0.60f, -0.55f, -0.15f, 0.25f};

        // The fault face. **Not dimmed**, deliberately and in contrast with `sad`: a fault is the
        // one thing that must not recede. One is the device being unhappy; the other is something
        // being wrong.
        case Emotion::kError:
            return FaceRecipe{0.95f, -0.75f, 0.85f, 0.0f, 0.0f};

        case Emotion::kCount:
            break;
    }
    return FaceRecipe{};
}

// Scale a recipe's expressiveness by `intensity`, 0..1.
//
// **Interpolation toward neutral, not multiplication.** Multiplying every field by `intensity`
// would drive `eye_openness` to zero at low intensity -- a barely-sad face would be a face with its
// eyes shut, which is a different expression entirely and a much stronger one. Blending toward the
// resting face is what "less of this emotion" actually means.
//
// `dim` is deliberately included: a slightly sad face should be slightly dimmed, not fully.
inline constexpr FaceRecipe withIntensity(const FaceRecipe& recipe, float intensity) {
    const float amount = clampUnit(intensity, 0.0f, 1.0f);
    const FaceRecipe rest = recipeFor(Emotion::kNeutral);
    const auto blend = [amount](float from, float to) { return from + (to - from) * amount; };
    return FaceRecipe{
        blend(rest.eye_openness, recipe.eye_openness),
        blend(rest.mouth_curve, recipe.mouth_curve),
        blend(rest.brow_angle, recipe.brow_angle),
        blend(rest.tilt, recipe.tilt),
        blend(rest.dim, recipe.dim),
    };
}

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

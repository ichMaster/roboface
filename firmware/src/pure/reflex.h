// The first level: a reaction that arrives before anyone could have thought about it.
//
// **A reflex is an overlay with a deadline, and both halves of that matter.**
//
// *Overlay*, because it modulates the current expression rather than replacing it. A reflex that
// targeted the crossfade would outlive itself: poke the device once and the face keeps whatever the
// poke did to it until the next `emotion{}` arrives, which on an idle device is never.
//
// *Deadline*, because a reflex is by definition a thing that stops. v2.3's review named the pattern
// this subsystem keeps producing -- **a signal that stops moving** -- and a reflex is the next
// candidate: it is the only animation here whose normal state is "not running".
//
// And it never interrupts speech or listening. Roadmap §v2.4 says so twice, and it is the reason
// the two levels exist at all: the mouth keeps its viseme, the level meter keeps its band, and the
// character answers in its own time on the second level.
//
// **Pure**: header-only, `namespace roboface`, no M5GFX, no clock of its own.

#pragma once

#include <cstdint>

#include "pure/face.h"

namespace roboface {

//: What a reflex does to the face. Deliberately few: a reflex is a punctuation mark, not a
//: sentence, and five of these cover every gesture DEVICE_UI §Input names.
enum class Reflex : uint8_t {
    kNone,
    kTickle,     // a tap: eyes crease, a quick smile
    kContented,  // a stroke: arc eyes, the face softens
    kStartle,    // a poke or a shake: eyes wide, brows up
    kDizzy,      // upside down or free fall: eyes wobble, the face dims
    kAlert,      // something approached: eyes widen slightly, gaze turns
    kCount,
};

//: How long each one lasts.
//:
//: **All short, and none the same.** A tickle is a flinch and is over before it registers as an
//: animation; a contented arc is the one a person lingers on, because they are still stroking. A
//: startle is longer than a tickle because being startled *takes* longer to stop.
//:
//: None of them approaches the ~3 s DEVICE_UI gives chrome to fade: a reflex that lasted that long
//: would be an expression, and expressions come from the server.
inline constexpr uint32_t reflexDurationMs(Reflex reflex) {
    switch (reflex) {
        case Reflex::kTickle: return 450;
        case Reflex::kContented: return 900;
        case Reflex::kStartle: return 700;
        case Reflex::kDizzy: return 1200;
        case Reflex::kAlert: return 600;
        case Reflex::kNone:
        case Reflex::kCount: break;
    }
    return 0;
}

//: How much a repeated tap adds, and how fast that fades.
//:
//: DEVICE_UI: *"repeated taps build joy"*. So the tickle deepens with the count -- but it is capped,
//: because a person tapping thirty times should not produce a face thirty times happier than one
//: tapping three, and it decays, because joy that never faded would leave the device permanently
//: delighted by something that happened yesterday.
inline constexpr float kAffectionPerTap = 0.25f;
inline constexpr float kAffectionCap = 1.0f;
inline constexpr uint32_t kAffectionFadeMs = 8000;

// A reflex, running.
//
// The caller asks for one, ticks it, and applies whatever it returns on top of the expression the
// crossfade produced. Nothing here knows what the expression *is* -- which is what makes a reflex
// composable with any of them, including the ones v2.6's skins will add.
class ReflexLayer {
  public:
    //: Start a reflex. A new one replaces whatever was running: two at once would be two things
    //: modulating one face, and the second would look like the first misbehaving.
    void fire(Reflex reflex, uint32_t now_ms) {
        if (reflex == Reflex::kNone) return;
        reflex_ = reflex;
        started_ms_ = now_ms;
        duration_ms_ = reflexDurationMs(reflex);
    }

    //: Record a tap for the affection that builds across them.
    void tapped(uint32_t now_ms) {
        affection_ = decayedAffection(now_ms) + kAffectionPerTap;
        if (affection_ > kAffectionCap) affection_ = kAffectionCap;
        affection_at_ms_ = now_ms;
    }

    //: Whether a reflex is currently running.
    bool isActive(uint32_t now_ms) const {
        return reflex_ != Reflex::kNone && now_ms - started_ms_ < duration_ms_;
    }

    Reflex current() const { return reflex_; }

    //: Accumulated affection, faded by time. 0..1.
    float affection(uint32_t now_ms) const { return decayedAffection(now_ms); }

    //: Apply the running reflex to a recipe. Returns it unchanged when nothing is running, which is
    //: almost always -- a reflex is punctuation.
    //:
    //: **`speaking` is not a suggestion.** While the device talks the mouth belongs to the lip-sync,
    //: and a reflex that moved it would fight an animation running twenty times a second. The eyes
    //: and brows are still free, so a poke during a reply is still visible -- it just does not take
    //: the mouth.
    FaceRecipe apply(const FaceRecipe& base, uint32_t now_ms, bool speaking = false) const {
        if (!isActive(now_ms)) return base;

        const float elapsed = static_cast<float>(now_ms - started_ms_);
        const float span = static_cast<float>(duration_ms_);
        // A reflex swells and subsides rather than switching on: a triangle over its own duration,
        // which at these lengths reads as a movement rather than as a jump.
        const float phase = elapsed / span;
        const float strength = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;

        FaceRecipe out = base;
        switch (reflex_) {
            case Reflex::kTickle: {
                // Deeper with the count, which is what "repeated taps build joy" means.
                const float joy = strength * (0.35f + 0.4f * decayedAffection(now_ms));
                out.eye_openness = base.eye_openness * (1.0f - 0.45f * strength);
                if (!speaking) out.mouth_curve = base.mouth_curve + joy;
                out.brow_angle = base.brow_angle - 0.2f * strength;
                break;
            }
            case Reflex::kContented:
                out.eye_openness = base.eye_openness * (1.0f - 0.55f * strength);
                if (!speaking) out.mouth_curve = base.mouth_curve + 0.3f * strength;
                out.tilt = base.tilt + 0.15f * strength;
                break;
            case Reflex::kStartle:
                out.eye_openness = base.eye_openness + (1.0f - base.eye_openness) * strength;
                out.brow_angle = base.brow_angle - 0.7f * strength;
                if (!speaking) out.mouth_curve = base.mouth_curve - 0.2f * strength;
                break;
            case Reflex::kDizzy:
                out.eye_openness = base.eye_openness * (1.0f - 0.3f * strength);
                out.tilt = base.tilt + 0.5f * strength;
                out.dim = base.dim + 0.25f * strength;
                break;
            case Reflex::kAlert:
                out.eye_openness = base.eye_openness + (1.0f - base.eye_openness) * 0.5f * strength;
                out.brow_angle = base.brow_angle - 0.3f * strength;
                break;
            case Reflex::kNone:
            case Reflex::kCount:
                break;
        }

        // Clamped for the same reason `layout.h` clamps: a face that inverted for one frame is very
        // visible, and a reflex is by construction something added on top of an unknown base.
        out.eye_openness = clampUnit(out.eye_openness, 0.0f, 1.0f);
        out.mouth_curve = clampUnit(out.mouth_curve, -1.0f, 1.0f);
        out.brow_angle = clampUnit(out.brow_angle, -1.0f, 1.0f);
        out.tilt = clampUnit(out.tilt, -1.0f, 1.0f);
        out.dim = clampUnit(out.dim, 0.0f, 1.0f);
        return out;
    }

    void clear() { reflex_ = Reflex::kNone; }

  private:
    float decayedAffection(uint32_t now_ms) const {
        if (affection_ <= 0.0f) return 0.0f;
        const uint32_t since = now_ms - affection_at_ms_;
        if (since >= kAffectionFadeMs) return 0.0f;
        const float remaining = 1.0f - static_cast<float>(since) /
                                           static_cast<float>(kAffectionFadeMs);
        return affection_ * remaining;
    }

    Reflex reflex_ = Reflex::kNone;
    uint32_t started_ms_ = 0;
    uint32_t duration_ms_ = 0;
    float affection_ = 0.0f;
    uint32_t affection_at_ms_ = 0;
};

}  // namespace roboface

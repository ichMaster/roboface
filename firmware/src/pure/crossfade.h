// A change of expression must read as movement, not as a cut.
//
// ARCHITECTURE §Renderer ladder gives 150-250 ms of crossfade; the roadmap adds `ttl_ms` relaxation
// back toward rest. Both are interpolation over `FaceRecipe`, which is why both live here and not
// in the renderer: five floats moving toward five other floats is arithmetic, and arithmetic is
// testable on a laptop.
//
// Two decisions in this file are worth more than they look:
//
// **Eased, not linear.** A linear blend of a face reads as a slide -- the features travel at a
// constant speed and arrive without settling. Ease-in-out gives the same duration a beginning and
// an end, which is the difference between a face that changed its expression and one that was
// dragged into a new one.
//
// **An interrupted crossfade restarts from where it is.** Not from its original source: if a state
// changes twice quickly and the second fade began from the first fade's *start*, the face would
// visibly jump backwards before moving on. This is the case that happens constantly in practice --
// idle to listening to thinking inside a second -- and never in a demo.

#pragma once

#include <cstdint>

#include "pure/face.h"

namespace roboface {

//: A state change. Long enough to be seen as motion, short enough that the face is never "in
//: between" when someone looks at it.
inline constexpr uint32_t kCrossfadeMs = 200;

//: The drift back to rest after a target's time is up. Much slower than a state change: relaxing is
//: something a face does on its own, and at 200 ms it would look like another decision.
inline constexpr uint32_t kRelaxMs = 900;

namespace detail {

inline constexpr float lerp(float from, float to, float t) { return from + (to - from) * t; }

//: Ease-in-out, cubic. Cheap -- two multiplies and a subtract -- and this runs once per frame, not
//: once per layer, so the shape is worth more here than the cycles it costs.
inline constexpr float ease(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

}  // namespace detail

//: Blend two recipes. Public because the renderer's lip-sync (v2.3) needs the same operation over
//: a mouth-only recipe, and a second implementation would drift from this one.
inline constexpr FaceRecipe blend(const FaceRecipe& from, const FaceRecipe& to, float t) {
    return FaceRecipe{
        detail::lerp(from.eye_openness, to.eye_openness, t),
        detail::lerp(from.mouth_curve, to.mouth_curve, t),
        detail::lerp(from.brow_angle, to.brow_angle, t),
        detail::lerp(from.tilt, to.tilt, t),
        detail::lerp(from.dim, to.dim, t),
    };
}

class Crossfade {
  public:
    Crossfade() = default;
    explicit Crossfade(const FaceRecipe& resting) : from_(resting), to_(resting), current_(resting) {}

    //: Aim at a new expression. `ttl_ms` of 0 means "hold it" -- the face stays until something
    //: else changes it; anything else starts the clock on relaxing back to `resting`.
    //:
    //: **Starts from the current face, not from `to_`.** That is what stops a rapid sequence of
    //: states from jumping backwards, and it is the whole reason `current_` is kept.
    void target(const FaceRecipe& next, uint32_t ttl_ms = 0) {
        // Too similar to be worth animating: `areDistinct` exists for exactly this, and a 200 ms
        // fade between two nearly identical faces is 200 ms of nothing happening.
        if (!areDistinct(current_, next)) {
            from_ = next;
            to_ = next;
            current_ = next;
            progress_ms_ = kCrossfadeMs;
            duration_ms_ = kCrossfadeMs;
        } else {
            from_ = current_;
            to_ = next;
            progress_ms_ = 0;
            duration_ms_ = kCrossfadeMs;
        }
        ttl_ms_ = ttl_ms;
        held_ms_ = 0;
        relaxing_ = false;
    }

    //: Where the face rests when nothing else is asked of it. Set once at construction in practice;
    //: v2.2 lets the server change what "rest" means.
    void setResting(const FaceRecipe& resting) { resting_ = resting; }

    //: Advance and return the face for this frame.
    FaceRecipe advance(uint32_t delta_ms) {
        if (progress_ms_ < duration_ms_) {
            progress_ms_ += delta_ms;
            if (progress_ms_ > duration_ms_) progress_ms_ = duration_ms_;
            const float t = duration_ms_ == 0
                                ? 1.0f
                                : static_cast<float>(progress_ms_) / static_cast<float>(duration_ms_);
            current_ = blend(from_, to_, detail::ease(t));
            return current_;
        }

        // Settled. If the expression was given a lifetime, count it down and then drift home.
        if (ttl_ms_ > 0 && !relaxing_) {
            held_ms_ += delta_ms;
            if (held_ms_ >= ttl_ms_) {
                relaxing_ = true;
                from_ = current_;
                to_ = resting_;
                progress_ms_ = 0;
                duration_ms_ = kRelaxMs;
                ttl_ms_ = 0;
            }
        }
        return current_;
    }

    const FaceRecipe& current() const { return current_; }
    bool isFading() const { return progress_ms_ < duration_ms_; }
    bool isRelaxing() const { return relaxing_ && isFading(); }

  private:
    FaceRecipe resting_{};
    FaceRecipe from_{};
    FaceRecipe to_{};
    FaceRecipe current_{};
    uint32_t progress_ms_ = 0;
    uint32_t duration_ms_ = 0;
    uint32_t ttl_ms_ = 0;
    uint32_t held_ms_ = 0;
    bool relaxing_ = false;
};

}  // namespace roboface

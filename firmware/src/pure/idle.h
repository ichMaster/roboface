// The face is alive even in silence.
//
// Three motions, and the phase's whole goal is that together they read as *someone there* rather
// than as an animation playing. Each is small enough to be invisible on its own; what makes the
// difference is that none of them is periodic in a way a person can predict.
//
// **Pure, and driven by an injected clock** -- the same discipline as `pure/vad.h` and
// `pure/ptt.h`. A blink schedule is almost entirely about time, and proving "blinks land between
// two and six seconds apart, forever" on hardware means watching a device for an hour.
//
// **Randomised but deterministic.** A blink on a fixed interval reads as a metronome and is worse
// than no blink at all; a blink from an unseeded random is untestable. So: a seeded generator, and
// a test asserting the whole sequence while a person still sees no pattern.

#pragma once

#include <cstdint>

namespace roboface {

//: How long the eyes stay shut.
//:
//: 200 ms rather than 120, and the reason is the frame rate rather than the blink. The face
//: repaints ten times a second, so a 120 ms blink can fall between two frames and never be drawn
//: at all -- a blink nobody sees is worse than no blink, because it is also paid for. 200 ms
//: always spans at least two frames, and is still fast enough not to read as sleepiness.
inline constexpr uint32_t kBlinkDurationMs = 200;

//: The band a blink interval is drawn from. Human resting rate is wider than this, but a companion
//: that goes eight seconds without blinking looks switched off.
inline constexpr uint32_t kBlinkMinIntervalMs = 2200;
inline constexpr uint32_t kBlinkMaxIntervalMs = 6000;

//: One breath, in and out. Slow enough that it is felt rather than watched.
inline constexpr uint32_t kBreathPeriodMs = 4200;
inline constexpr float kBreathAmplitudePx = 3.0f;

//: The gaze wanders on its own, slower than the breath and over a smaller distance. This is the
//: motion people cannot name afterwards but notice the absence of.
inline constexpr uint32_t kGazePeriodMs = 9000;
inline constexpr float kGazeAmplitudePx = 4.0f;

//: What the idle loop contributes to a frame. Offsets over the layer bank, never a replacement for
//: it: the recipe still says what the face is doing, and this says how it is breathing while it
//: does that.
struct IdleOffsets {
    //: Multiplies eye openness. 1 = the recipe's own value, 0 = shut for a blink.
    float eye_scale = 1.0f;
    //: Vertical bob, in pixels, applied to the features.
    float bob_y = 0.0f;
    //: Where the gaze has drifted, in pixels.
    float gaze_x = 0.0f;
    float gaze_y = 0.0f;
};

//: A small deterministic generator. Not `rand()`: the standard one carries global state, so two
//: instances would interfere and a test could not reproduce a sequence. Sixteen bits is plenty for
//: choosing a blink interval and costs nothing on this chip.
class BlinkRandom {
  public:
    explicit constexpr BlinkRandom(uint32_t seed = 0x5EEDu) : state_(seed | 1u) {}

    //: xorshift32 -- one multiply-free step, uniform enough for "when is the next blink".
    constexpr uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    //: Inclusive of both ends, and safe when they are equal -- a caller that pins the interval for
    //: a test should get exactly that rather than a division by zero.
    constexpr uint32_t between(uint32_t low, uint32_t high) {
        if (high <= low) return low;
        return low + (next() % (high - low + 1u));
    }

  private:
    uint32_t state_;
};

class IdleLoop {
  public:
    explicit IdleLoop(uint32_t seed = 0x5EEDu) : random_(seed) {
        next_blink_ms_ = random_.between(kBlinkMinIntervalMs, kBlinkMaxIntervalMs);
    }

    //: 0 stills the *drift* -- the breath and the gaze -- and 1 is full motion. Blinking is
    //: deliberately **not** scaled by this: see `setBlinking`.
    //:
    //: Scales without the loop knowing what an emotion is, which is what keeps v2.2 an addition
    //: rather than a rewrite.
    void setIntensity(float intensity) {
        intensity_ = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    }
    float intensity() const { return intensity_; }

    //: Blinking, separately from the drift, and separate for a reason a person can see.
    //:
    //: A face that is listening should be **still and attentive**, not wandering -- the drift reads
    //: as distraction exactly when the device is supposed to be paying attention. But a face that
    //: stops blinking entirely stops looking alive and starts looking frozen, which is how a person
    //: tells a picture from a device.
    //:
    //: So listening stills the drift and keeps the blink. Two controls because they answer two
    //: different questions.
    void setBlinking(bool enabled) { blinking_enabled_ = enabled; }

    //: Start again from rest. Used when the face has been doing something else and the idle should
    //: not resume mid-blink.
    void reset() {
        elapsed_ms_ = 0;
        blink_started_ms_ = 0;
        blinking_ = false;
        next_blink_ms_ = random_.between(kBlinkMinIntervalMs, kBlinkMaxIntervalMs);
    }

    //: Advance by `delta_ms` and return what this frame should add. Time is passed in rather than
    //: read, so an hour of idling runs in a millisecond on a laptop.
    IdleOffsets advance(uint32_t delta_ms) {
        elapsed_ms_ += delta_ms;

        if (blinking_) {
            if (elapsed_ms_ - blink_started_ms_ >= kBlinkDurationMs) {
                blinking_ = false;
                // The next interval is drawn when the previous blink *ends*, not when it starts, so
                // the gap between blinks is what the band describes rather than the gap plus a
                // blink's own length.
                next_blink_ms_ = elapsed_ms_ + random_.between(kBlinkMinIntervalMs,
                                                               kBlinkMaxIntervalMs);
            }
        } else if (elapsed_ms_ >= next_blink_ms_) {
            blinking_ = true;
            blink_started_ms_ = elapsed_ms_;
        }

        IdleOffsets offsets;
        if (blinking_ && blinking_enabled_) {
            // Shut, not squinting: a partial blink at this size looks like a rendering fault, so
            // this is 0 or 1 and never in between.
            offsets.eye_scale = 0.0f;
        }
        offsets.bob_y = intensity_ * kBreathAmplitudePx * wave(elapsed_ms_, kBreathPeriodMs);
        offsets.gaze_x = intensity_ * kGazeAmplitudePx * wave(elapsed_ms_, kGazePeriodMs);
        // A different phase and a longer period on the vertical, so the gaze wanders rather than
        // tracing a diagonal back and forth.
        offsets.gaze_y = intensity_ * (kGazeAmplitudePx * 0.6f) *
                         wave(elapsed_ms_ + kGazePeriodMs / 3u, kGazePeriodMs * 7u / 5u);
        return offsets;
    }

    bool isBlinking() const { return blinking_; }
    uint32_t elapsedMs() const { return elapsed_ms_; }

  private:
    //: A triangle wave in -1..1, not a sine. `sinf` on this chip is a call into libm on every
    //: layer of every frame; a triangle is three comparisons and, once the motion is three pixels
    //: over four seconds, nobody can tell them apart.
    static constexpr float wave(uint32_t elapsed_ms, uint32_t period_ms) {
        if (period_ms == 0) return 0.0f;
        const uint32_t phase = elapsed_ms % period_ms;
        const float unit = static_cast<float>(phase) / static_cast<float>(period_ms);  // 0..1
        // 0 -> 0, 0.25 -> 1, 0.5 -> 0, 0.75 -> -1
        if (unit < 0.25f) return unit * 4.0f;
        if (unit < 0.75f) return 2.0f - unit * 4.0f;
        return unit * 4.0f - 4.0f;
    }

    BlinkRandom random_;
    float intensity_ = 1.0f;
    uint32_t elapsed_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_started_ms_ = 0;
    bool blinking_ = false;
    bool blinking_enabled_ = true;
};

}  // namespace roboface

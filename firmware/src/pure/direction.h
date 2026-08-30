// Where the voice is, from two channels — and how much to believe it.
//
// **The physics decides the design here, and it rules out the obvious approach.** The textbook way
// to locate a source with two microphones is the time difference between them. On this board that
// difference cannot be measured:
//
//     microphone spacing on a Core S3   ~40 mm
//     speed of sound                     343 m/s
//     maximum arrival difference         40 mm / 343 m/s  =  117 us
//     one sample at 16 kHz                              =   62.5 us
//     => the entire left-to-right range spans **±2 samples**
//
// Two samples is not a measurement, it is a coin toss with reverberation on it. So the time cue is
// not used for direction at all, and pretending otherwise would produce a confident number with
// nothing under it.
//
// What the two channels *do* give, at any sample rate:
//
//   * **level balance** -> direction. The nearer microphone is louder. Coarse, honest, and
//     unaffected by the sample rate.
//   * **coherence** -> confidence. Two microphones hearing one source see the *same waveform*;
//     two microphones in a room full of fan noise, traffic and reflections see two different ones.
//     Normalised cross-correlation at zero lag separates those, and it is the single most useful
//     thing a second microphone provides. It is also what tells a face not to look at a fan.
//
// So: direction from level, confidence from coherence, and **absent** whenever confidence is low --
// which is a third state, not a centred gaze. `coerce_gaze` on the server already draws that
// distinction ("no opinion" and "look straight ahead" are different), and this is the device end of
// the same idea.
//
// Pure: header-only, `namespace roboface`, no <M5Unified.h>, no clock of its own.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/roots.h"
#include "pure/stereo.h"

namespace roboface {

// --------------------------------------------------------------------------------------
// The thresholds, each with the number it came from
// --------------------------------------------------------------------------------------

//: How correlated the two channels must be before the estimate is believed at all.
//:
//: One source reaching both microphones 117 us apart produces channels that are nearly identical --
//: correlation well above 0.9 in a quiet room. Diffuse noise produces correlation scattered around
//: zero. 0.55 sits between them with room on both sides, and errs toward silence: a face that looks
//: at nothing is unremarkable, a face that tracks a fan is a broken device.
inline constexpr float kCoherenceThreshold = 0.55f;

//: Below this RMS there is nothing to have an opinion about. Two noise floors are perfectly capable
//: of correlating with each other.
inline constexpr float kDirectionSilenceRms = 0.010f;

//: How far the balance must swing for the direction to be worth acting on. Measured on the board:
//: ambient imbalance sits near 0.04 and a voice close to one side reaches past 0.19. 0.12 is above
//: the room and below a person.
inline constexpr float kDirectionDeadZone = 0.12f;

//: Level balance is a compressed measure of angle -- a source hard to one side gives roughly 0.3,
//: not 1.0, because both microphones still hear it well. This scales the useful range out to the
//: full [-1, +1] that `Gaze` expects, and the estimate is clamped after it.
inline constexpr float kBalanceToDirection = 3.0f;

//: How fast the estimate follows a new speaker, and how slowly it lets go. **Asymmetric on purpose.**
//: A face that snapped back to centre in every pause between words would look nervous rather than
//: attentive; a face slow to turn would look deaf. So: quick to move, reluctant to forget.
inline constexpr float kDirectionRise = 0.35f;
inline constexpr float kDirectionFall = 0.06f;

//: How long a confident estimate stands after the voice stops before the direction is dropped
//: entirely. Long enough to survive a breath between sentences.
inline constexpr uint32_t kDirectionHoldMs = 1500;

//: Normalised cross-correlation of two channels at zero lag, in [-1, +1].
//:
//: **The measure that separates a person from a room.** 1.0 means the channels carry the same
//: waveform, which at this microphone spacing is what one source sounds like; near 0 means they
//: carry unrelated sound, which is what a room sounds like.
//:
//: No <cmath> -- `pure/roots.h` instead, so a threshold tested on a laptop is the threshold that
//: runs on the board.
inline float coherence(const int16_t* left, const int16_t* right, std::size_t count) {
    if (left == nullptr || right == nullptr || count == 0) return 0.0f;

    int64_t dot = 0;
    int64_t left_energy = 0;
    int64_t right_energy = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int64_t l = left[i];
        const int64_t r = right[i];
        dot += l * r;
        left_energy += l * l;
        right_energy += r * r;
    }
    if (left_energy == 0 || right_energy == 0) return 0.0f;

    // **`squareRoot`, not a Newton loop written here.** The product of two frame energies is
    // routinely around 1e20, and the naive "start at the value, iterate 32 times" that the rest of
    // this project uses does not converge from there -- it returned 0.6 for two identical channels,
    // which is exactly the wrong answer in the wrong direction: it makes a person look like a room.
    const float product = static_cast<float>(left_energy) * static_cast<float>(right_energy);
    const float root = squareRoot(product);
    if (root <= 0.0f) return 0.0f;
    return static_cast<float>(dot) / root;
}

//: What the estimator concluded from one frame.
struct DirectionEstimate {
    //: Where the voice is, -1 hard left to +1 hard right. Meaningless unless `present`.
    float direction = 0.0f;

    //: How much of a single source this looked like: the coherence, passed through.
    float confidence = 0.0f;

    //: **Whether there is an opinion at all.** Not the same as `direction == 0`, and the difference
    //: is the whole point: a centred speaker and an empty room must not produce the same answer,
    //: because one of them should move the gaze and the other must leave the idle drift alone.
    bool present = false;
};

// Two channels in, one smoothed direction out.
//
// Holds only what a decision needs across frames -- the smoothed value and when it was last
// confident. Time arrives as a parameter, so a whole conversation can be replayed in microseconds.
class DirectionEstimator {
  public:
    DirectionEstimate feed(const int16_t* left, const int16_t* right, std::size_t count,
                           uint32_t now_ms) {
        const StereoLevels levels = measure(left, right, count);
        const float loudest = levels.left > levels.right ? levels.left : levels.right;

        DirectionEstimate result;
        result.confidence = coherence(left, right, count);

        const bool audible = loudest >= kDirectionSilenceRms;
        const bool coherent = result.confidence >= kCoherenceThreshold;
        const float swing = levels.balance < 0.0f ? -levels.balance : levels.balance;

        if (audible && coherent && swing >= kDirectionDeadZone) {
            float target = levels.balance * kBalanceToDirection;
            if (target > 1.0f) target = 1.0f;
            if (target < -1.0f) target = -1.0f;
            smoothed_ += (target - smoothed_) * kDirectionRise;
            confident_at_ms_ = now_ms;
            has_held_ = true;
        } else if (has_held_ && now_ms - confident_at_ms_ < kDirectionHoldMs) {
            // Still holding the last opinion, letting it relax rather than snapping to centre. A
            // pause between words is not a person leaving the room.
            smoothed_ += (0.0f - smoothed_) * kDirectionFall;
        } else {
            has_held_ = false;
            smoothed_ = 0.0f;
        }

        result.direction = smoothed_;
        result.present = has_held_;
        return result;
    }

    //: Forget the current opinion. Called when the device stops listening -- a direction learned
    //: before a reply is not evidence about the room after it.
    void reset() {
        smoothed_ = 0.0f;
        has_held_ = false;
        confident_at_ms_ = 0;
    }

  private:
    float smoothed_ = 0.0f;
    bool has_held_ = false;
    uint32_t confident_at_ms_ = 0;
};

}  // namespace roboface

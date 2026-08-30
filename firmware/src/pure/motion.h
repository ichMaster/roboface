// What the device is being done to, from three numbers and a clock.
//
// The accelerometer reports the sum of gravity and whatever else is pushing on the device. At rest
// that sum has magnitude 1 g and points at the floor, and **every motion below is a way of saying
// how it stopped doing that**:
//
//   tilt          the direction moved, the magnitude did not
//   free fall     the magnitude collapsed toward zero -- nothing is holding it up
//   upside down   the direction inverted and stayed there
//   shake         the direction alternated faster than a hand moves a thing on purpose
//   picked up     the magnitude departed 1 g and then settled somewhere new
//
// Stating them that way rather than as five independent detectors is what keeps the thresholds
// honest: each one is a number about gravity, and gravity is 9.81 m/s² wherever the desk is.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>, no clock of its own. Samples and
// timestamps arrive as parameters, so a drop can be replayed on a laptop without dropping anything.

#pragma once

#include <cstdint>

namespace roboface {

//: What happened, as ARCHITECTURE §event{} names it. `kNone` is a device sitting on a desk, which
//: is what it is doing almost always -- and the case that matters most, because a false positive
//: fires when nobody is there to explain it away.
enum class Motion : uint8_t {
    kNone,
    kTilt,
    kShake,
    kPickedUp,
    kUpsideDown,
    kFreeFall,
    kCount,
};

inline constexpr const char* toString(Motion motion) {
    switch (motion) {
        case Motion::kNone: return "none";
        case Motion::kTilt: return "tilt";
        case Motion::kShake: return "shake";
        case Motion::kPickedUp: return "picked_up";
        case Motion::kUpsideDown: return "upside_down";
        case Motion::kFreeFall: return "free_fall";
        case Motion::kCount: break;
    }
    return "none";
}

//: One accelerometer sample, in **g** -- which is what `M5.Imu.getAccel` reports, so nothing is
//: converted on the way in and no scale factor can be got wrong in the glue.
struct AccelSample {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t at_ms = 0;
};

// --------------------------------------------------------------------------------------
// The thresholds, each with the number it came from
// --------------------------------------------------------------------------------------

//: Below this magnitude nothing is holding the device up. **True free fall is 0 g**; 0.35 leaves
//: room for the tumble a real drop has -- a phone dropped flat reads 0.1-0.3 g, never a clean zero,
//: because it rotates on the way down and the sensor is not at the centre of mass.
inline constexpr float kFreeFallG = 0.35f;

//: How long that must hold before it is a fall rather than a jolt. A desk is ~70 cm, so a fall
//: lasts about 380 ms; 120 ms is a third of that -- long enough that a sharp downward shove does
//: not qualify, short enough to react before it lands.
inline constexpr uint32_t kFreeFallMs = 120;

//: Gravity pointing up rather than down. −0.7 g on the screen axis is about 45° past horizontal:
//: unambiguous, and unreachable by a device standing on a desk at any tilt.
inline constexpr float kUpsideDownG = -0.7f;

//: How far the gravity direction must move to be a tilt. 0.25 g on an axis is roughly 15°, which
//: is more than a desk knock and less than a deliberate lean.
inline constexpr float kTiltG = 0.25f;

//: And back within this to stop being tilted. The gap is the hysteresis, for the reason
//: `lipsync.h` documents: a value sitting on a threshold flips on adjacent samples, and a face
//: reacting to that would twitch rather than respond.
inline constexpr float kTiltReleaseG = 0.15f;

//: A shake is the total acceleration swinging past this. 1.8 g is roughly twice gravity -- a
//: deliberate movement, not a bump against the desk, which peaks near 1.3.
inline constexpr float kShakeG = 1.8f;

//: How many reversals inside the window make it a shake rather than one jolt. Three, because two
//: is "moved it somewhere" and four would need a long enough window that a slow wave qualifies.
inline constexpr uint8_t kShakeReversals = 3;

//: The window those must land in. 600 ms is about three shakes of a wrist; longer and two
//: unrelated knocks a second apart would merge into a shake nobody performed.
inline constexpr uint32_t kShakeWindowMs = 600;

//: How far from 1 g the magnitude must go, and then settle, to be "picked up". Lifting something
//: takes it to 1.2-1.5 g on the way up and back to 1 g at a new angle.
inline constexpr float kPickUpG = 0.30f;

//: How long it must be quiet again before the lift is over rather than still happening.
inline constexpr uint32_t kMotionSettleMs = 250;

//: Magnitude of a sample, in g. No `sqrt` from <cmath>: this header compiles for both the host and
//: an ESP32-S3, and the same Newton iteration `envelope.h` uses is exact enough for a threshold
//: comparison and identical on both.
inline float magnitude(const AccelSample& sample) {
    const float squared = sample.x * sample.x + sample.y * sample.y + sample.z * sample.z;
    if (squared <= 0.0f) return 0.0f;
    float root = squared > 1.0f ? squared : 1.0f;
    for (int i = 0; i < 20; ++i) {
        const float next = 0.5f * (root + squared / root);
        if (next == root) break;
        root = next;
    }
    return root;
}

// Turn a stream of accelerometer samples into named motions.
//
// **One motion at a time, and the order is a priority.** A device in free fall is also, briefly,
// being shaken and tilted; reporting all three would be true and useless. Free fall outranks
// everything because it is the only one that matters in the next 200 ms.
class MotionDetector {
  public:
    //: Feed one sample. Returns a motion on the sample that completes one, `kNone` otherwise.
    Motion feed(const AccelSample& sample) {
        const float total = magnitude(sample);

        // --- free fall: nothing is holding it up ---------------------------------------
        if (total < kFreeFallG) {
            if (falling_since_ms_ == 0) falling_since_ms_ = sample.at_ms;
            if (!reported_fall_ && sample.at_ms - falling_since_ms_ >= kFreeFallMs) {
                reported_fall_ = true;
                return Motion::kFreeFall;
            }
            return Motion::kNone;
        }
        falling_since_ms_ = 0;
        reported_fall_ = false;

        // --- upside down: gravity points the wrong way ----------------------------------
        const bool inverted = sample.z < kUpsideDownG;
        if (inverted && !was_inverted_) {
            was_inverted_ = true;
            return Motion::kUpsideDown;
        }
        if (!inverted && sample.z > 0.0f) was_inverted_ = false;

        // --- shake: the total swinging past twice gravity, repeatedly -------------------
        const bool beyond = total > kShakeG;
        if (beyond && !was_beyond_) {
            if (sample.at_ms - shake_window_ms_ > kShakeWindowMs) {
                shake_window_ms_ = sample.at_ms;
                reversals_ = 0;
            }
            ++reversals_;
            if (reversals_ >= kShakeReversals) {
                reversals_ = 0;
                shake_window_ms_ = sample.at_ms;
                was_beyond_ = beyond;
                return Motion::kShake;
            }
        }
        was_beyond_ = beyond;

        // --- picked up: the magnitude left 1 g and then settled -------------------------
        const float from_rest = total > 1.0f ? total - 1.0f : 1.0f - total;
        if (from_rest > kPickUpG) {
            lifting_ = true;
            quiet_since_ms_ = 0;
        } else if (lifting_) {
            if (quiet_since_ms_ == 0) quiet_since_ms_ = sample.at_ms;
            if (sample.at_ms - quiet_since_ms_ >= kMotionSettleMs) {
                lifting_ = false;
                quiet_since_ms_ = 0;
                // A lift ends where a tilt would be measured from, so the reference moves with it.
                rest_x_ = sample.x;
                rest_y_ = sample.y;
                return Motion::kPickedUp;
            }
        }

        // --- tilt: the direction moved, the magnitude did not ---------------------------
        if (!lifting_) {
            const float dx = sample.x - rest_x_;
            const float dy = sample.y - rest_y_;
            const float moved = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (!tilted_ && moved > kTiltG) {
                tilted_ = true;
                return Motion::kTilt;
            }
            if (tilted_ && moved < kTiltReleaseG) tilted_ = false;
        }

        return Motion::kNone;
    }

    //: Take the current sample as level. Called at boot: a device on a shelf at an angle is not
    //: permanently tilted, it is a device on a shelf.
    void level(const AccelSample& sample) {
        rest_x_ = sample.x;
        rest_y_ = sample.y;
        tilted_ = false;
    }

  private:
    float rest_x_ = 0.0f;
    float rest_y_ = 0.0f;
    bool tilted_ = false;
    bool was_inverted_ = false;
    bool was_beyond_ = false;
    bool lifting_ = false;
    bool reported_fall_ = false;
    uint8_t reversals_ = 0;
    uint32_t shake_window_ms_ = 0;
    uint32_t falling_since_ms_ = 0;
    uint32_t quiet_since_ms_ = 0;
};

}  // namespace roboface

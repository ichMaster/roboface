// Something is close, or it stopped being close.
//
// The LTR-553 reports a raw proximity count that **rises as an object gets nearer** — it is a
// reflected-light reading, not a distance, so it has no units and its scale depends on what is
// reflecting. That is why the thresholds here are stated as counts and not as centimetres: a
// number in centimetres would be a conversion nobody can check, and a pale hand and a dark sleeve
// at the same distance do not read the same.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>, no clock of its own.

#pragma once

#include <cstdint>

namespace roboface {

enum class Presence : uint8_t {
    kNone,
    kApproach,
    kLeave,
    kCount,
};

inline constexpr const char* toString(Presence presence) {
    switch (presence) {
        case Presence::kNone: return "none";
        case Presence::kApproach: return "approach";
        case Presence::kLeave: return "leave";
        case Presence::kCount: break;
    }
    return "none";
}

//: Above this count something is near. Roughly a hand at 10 cm on this sensor; the exact distance
//: depends on what is reflecting, which is why the constant is a count.
inline constexpr uint16_t kNearCount = 300;

//: And below this it has gone. **The gap is the whole point** -- a hand hovering at the boundary
//: would otherwise produce a stream of approach/leave pairs, and the face would flicker between
//: waking and relaxing several times a second.
inline constexpr uint16_t kFarCount = 180;

//: How long a reading must hold before it counts. The sensor is noisy and a sleeve passing over it
//: is not an approach; 150 ms is shorter than a deliberate reach and longer than a wave.
inline constexpr uint32_t kProximitySettleMs = 150;

// Approach and leave, debounced.
class ProximityDetector {
  public:
    //: Feed one reading. Returns an event on the sample that confirms a change, `kNone` otherwise.
    Presence feed(uint16_t count, uint32_t at_ms) {
        const bool near = near_ ? count > kFarCount : count > kNearCount;

        if (near == near_) {
            candidate_since_ms_ = 0;
            return Presence::kNone;
        }

        // The reading disagrees with the current state. Give it time to mean it.
        if (candidate_since_ms_ == 0) {
            candidate_since_ms_ = at_ms;
            return Presence::kNone;
        }
        if (at_ms - candidate_since_ms_ < kProximitySettleMs) return Presence::kNone;

        near_ = near;
        candidate_since_ms_ = 0;
        return near ? Presence::kApproach : Presence::kLeave;
    }

    bool isNear() const { return near_; }

    void reset() {
        near_ = false;
        candidate_since_ms_ = 0;
    }

  private:
    bool near_ = false;
    uint32_t candidate_since_ms_ = 0;
};

//: How far the gaze is pulled toward something that has approached, as a fraction of full travel.
//:
//: Not all the way: the device looks *toward* a hand, it does not stare at it. 0.6 is a clear turn
//: that still reads as attention rather than as a lock.
inline constexpr float kGazePull = 0.6f;

}  // namespace roboface

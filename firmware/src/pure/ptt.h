// Press-and-hold: the trigger that survives every later version.
//
// **Pure**, and driven by an injected clock, the way `pure/chrome.h`'s fade rules are. The rules
// here are almost entirely about time, and time is the one thing a device test cannot rush: proving
// "a 100 ms tap is not an utterance" on hardware means tapping the glass for exactly 100 ms.
//
// **A release before the threshold is a tap, not a short utterance.** A window that opened and
// closed in 80 ms would send `listen_start` and `listen_stop` with nothing between them, and v1.3
// would have to special-case an empty transcript for something the person never meant to say.
//
// The machine reports **transitions**, not levels, so the caller sends `listen_start` exactly once
// per hold. A caller polling a boolean would have to remember what it last saw, which is the same
// state kept in a second place -- and the two would eventually disagree.

#pragma once

#include <cstdint>

namespace roboface {

//: How long a press must last before it means "listen". ROADMAP §v1.2 gives ~120 ms, which is long
//: enough that a tap on the way past does not open a window and short enough that a person who
//: meant to talk never notices the wait.
inline constexpr uint32_t kPttHoldMs = 120;

enum class PttEvent {
    kNone,     // nothing changed
    kStarted,  // the hold passed the threshold -- open the window
    kStopped,  // the hold ended -- close it
    kTapped,   // pressed and released before the threshold -- not an utterance
};

class PushToTalk {
  public:
    explicit PushToTalk(uint32_t hold_ms = kPttHoldMs) : hold_ms_(hold_ms) {}

    // Feed the panel's current state every loop. `pressed` is a level; the return is an edge.
    PttEvent update(bool pressed, uint32_t now_ms) {
        if (pressed) {
            if (!pressed_) {
                pressed_ = true;
                pressed_at_ms_ = now_ms;
                return PttEvent::kNone;
            }
            // Still down. The threshold is crossed once, and only once: `holding_` is what stops a
            // long press from reporting kStarted on every loop for as long as a finger is there.
            if (!holding_ && now_ms - pressed_at_ms_ >= hold_ms_) {
                holding_ = true;
                return PttEvent::kStarted;
            }
            return PttEvent::kNone;
        }

        if (!pressed_) return PttEvent::kNone;  // a release with no press: nothing was open

        pressed_ = false;
        if (holding_) {
            holding_ = false;
            return PttEvent::kStopped;
        }
        return PttEvent::kTapped;
    }

    // Give up any hold in progress without reporting a stop -- for a fault or a disconnect, where
    // the window is already gone and telling the caller to close it again would be a second close.
    void cancel() {
        pressed_ = false;
        holding_ = false;
    }

    bool isHolding() const { return holding_; }

  private:
    uint32_t hold_ms_;
    uint32_t pressed_at_ms_ = 0;
    bool pressed_ = false;
    bool holding_ = false;
};

}  // namespace roboface

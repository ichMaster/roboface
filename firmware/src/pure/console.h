// The console mode's state discipline: what it borrows, and giving it back.
//
// **Pure**, because the part that goes wrong is not the printing -- it is forgetting what the
// screen was showing before. v0.4's `/faces` self-test shipped that exact bug and had to grow a
// `self_test_saved_state` to fix it: without one, the cycle ended leaving the device in `error`,
// so the status line and the panel disagreed from then on. This is the same borrowing, so it gets
// the same discipline, in a form a host test can prove is total over the state enum.

#pragma once

#include "pure/state.h"

namespace roboface {

// Tracks whether the console is on, and the device state it took the screen from.
class ConsoleMode {
  public:
    bool isOn() const { return on_; }

    // Take the screen. Returns false if the console was already on -- entering twice must not
    // overwrite the saved state with the state the console itself is displaying, which would make
    // leaving restore the wrong thing.
    bool enable(DeviceState current) {
        if (on_) return false;
        saved_ = current;
        on_ = true;
        return true;
    }

    // Give it back. Returns false if it was already off, so a stray `/chat-off` is a no-op rather
    // than a state change.
    bool disable() {
        if (!on_) return false;
        on_ = false;
        return true;
    }

    // The state to restore. Meaningful only after `enable`; defaults to idle so a disable that
    // somehow runs first cannot return a garbage state.
    DeviceState savedState() const { return saved_; }

  private:
    bool on_ = false;
    DeviceState saved_ = DeviceState::kIdle;
};

}  // namespace roboface

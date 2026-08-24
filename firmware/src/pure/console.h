// The console mode: whether the transcript owns the screen.
//
// **Pure**, and deliberately thin. An earlier version of this type also saved the device state at
// `/chat-on` and put it back at `/chat-off`, by analogy with the `/faces` self-test. That analogy
// was wrong and the v0.5 review caught it: the self-test *drives* state -- it assigns each of the
// six in turn -- so it must restore the real one afterwards. The console drives nothing. The state
// machine keeps running underneath it, so a saved state goes stale the moment anything happens, and
// restoring it **reverts a transition that was legitimate**: open the console while idle, lose the
// link, leave the console, and the device would be put back to `idle` with no socket. The panel
// would then assert a state the device is not in, which is the rule MISSION exists to protect.
//
// So: the console borrows the *screen*. Leaving it draws whatever state the device is genuinely in.

#pragma once

namespace roboface {

class ConsoleMode {
  public:
    bool isOn() const { return on_; }

    // Take the screen. False if it was already taken, so a second `/chat-on` is a no-op rather than
    // a state change worth announcing.
    bool enable() {
        if (on_) return false;
        on_ = true;
        return true;
    }

    // Give it back. False if it was already off, so a stray `/chat-off` is a no-op.
    bool disable() {
        if (!on_) return false;
        on_ = false;
        return true;
    }

  private:
    bool on_ = false;
};

}  // namespace roboface

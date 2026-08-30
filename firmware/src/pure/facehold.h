// How long the server's last word stands, and when the mouth is allowed to move.
//
// Two small decisions that would be invisible inside the renderer and are the two most likely to
// be wrong. Both are pure, so both are answerable on a laptop rather than by watching a screen.
//
// Pure: header-only, `namespace roboface`, no M5GFX, host-tested.

#pragma once

#include <cstdint>

namespace roboface {

// A frame's time to live, counted down.
//
// **A liveness guarantee, not a schedule.** The server sends an `emotion{}` on every state change,
// so in normal operation a new frame always arrives before the old one expires and this class does
// nothing at all. What it bounds is the abnormal case: the connection dying between two state
// changes, leaving the device wearing an expression about a turn that will never finish. A face
// stuck in `thinking` over a dead socket is the failure this exists to prevent, and it is a failure
// that looks exactly like the device working.
class FaceHold {
  public:
    //: A frame arrived. Restarts the countdown -- every frame is the server speaking again.
    void hold(uint32_t ttl_ms) { remaining_ms_ = ttl_ms; }

    //: Nothing from the server is in force. Not the same as an expired hold: this is what a
    //: device-local face leaves behind, and `boot` and `offline` are not things that expire.
    void release() { remaining_ms_ = 0; }

    bool isHeld() const { return remaining_ms_ > 0; }

    uint32_t remaining() const { return remaining_ms_; }

    //: Advance by `delta_ms`. Returns **true on the single tick the hold expires**, never after.
    //:
    //: An edge rather than a level, because the caller's response is a one-off -- crossfade back to
    //: neutral, restart the idle loop. A predicate that stayed true would re-target the crossfade on
    //: every frame afterwards and the face would never finish relaxing.
    bool advance(uint32_t delta_ms) {
        if (remaining_ms_ == 0) return false;
        if (delta_ms >= remaining_ms_) {
            remaining_ms_ = 0;
            return true;
        }
        remaining_ms_ -= delta_ms;
        return false;
    }

  private:
    uint32_t remaining_ms_ = 0;
};

// Should the mouth be moving?
//
// **Both halves, and this is the `v2.1.2` rule written down.** `server_permits` is the `speaking`
// flag on an `EmotionFrame`: the server saying a reply is being spoken. `speaker_playing` is the
// device's own audio path: the speaker saying it still is.
//
// Believing only the server froze the mouth mid-reply, and did so for a reason no amount of staring
// at the renderer would reveal — the device buffers seconds of audio the server has already
// finished sending, so the server's "the reply is over" arrives while the device is still saying
// it. Believing only the speaker would move the mouth for a loopback recording or a chime, neither
// of which is the character talking.
//
// So: the server grants permission, the speaker supplies the fact, and the mouth needs both.
inline constexpr bool mouthRuns(bool server_permits, bool speaker_playing) {
    return server_permits && speaker_playing;
}

}  // namespace roboface

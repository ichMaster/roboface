// The renderer seam.
//
// ARCHITECTURE §The face → Renderer ladder names these four methods, and fixing them **now** --
// while the renderer behind them is a stub -- is the point of v0.4. v2's animated renderer
// implements this interface rather than inventing one, and v2.6's skins swap behind it. An
// interface agreed against a stub is one later versions extend; an interface discovered while
// writing the real thing is one everything else has to be rewritten around.
//
// `setAudioLevel` exists here and does nothing until v1's lip-sync (ARCHITECTURE §The face →
// Lip-sync: the level is local, derived from the playback buffer, and deliberately *not* part of
// `EmotionFrame`). Declaring it now is what stops v1 widening the interface.

#pragma once

#include <cstdint>

#include "pure/face.h"
#include "pure/state.h"

namespace app {

class IFaceRenderer {
  public:
    virtual ~IFaceRenderer() = default;

    // Set up the panel and any buffers. Returns false if it could not -- a renderer that failed to
    // allocate must say so rather than presenting a blank screen as a working one.
    virtual bool begin() = 0;

    // Show the face for a device state. **The device-state form stays, and that is the authority
    // rule rather than a transitional convenience**: `boot`, `wifi_connecting`, `offline` and a
    // local fault are facts about the link -- in the offline case, by definition facts about the
    // server being unreachable -- so a device that waited to be told how to look while
    // disconnected would have no face at all.
    //
    // What v2.2 took away is the *turn* states. `listening`, `thinking` and `replying` arrive as
    // frames now; the device no longer infers an expression from its own state machine.
    virtual void show(roboface::DeviceState state) = 0;

    // Show the face the server sent (v2.2). This is the overload the seam was written to gain,
    // and the one the phase moves the last decision onto: the device renders what it is given.
    //
    // The frame stands for `ttl_ms`; with nothing after it the renderer relaxes to `neutral`. That
    // is a liveness guarantee rather than a schedule -- in normal operation a new frame always
    // arrives first, and what it bounds is the connection dying mid-turn.
    virtual void show(const roboface::EmotionFrame& frame) = 0;

    // 0..1, from the playback buffer. Does nothing until v1.
    virtual void setAudioLevel(float level) = 0;

    // Called every loop. The stub has nothing to animate; v2's renderer runs its idle loop,
    // crossfades and ttl expiry here.
    virtual void tick(uint32_t now_ms) = 0;
};

}  // namespace app

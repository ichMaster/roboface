// The face, drawn.
//
// This class owns buffers, the panel and nothing else. Every decision it needs was made in
// `src/pure/`: `layers.h` turned a recipe into shapes, `crossfade.h` decided which recipe this
// frame is, `idle.h` said how it is breathing while it does that. What is left here is putting
// shapes on a screen -- which is the only part a host cannot check, and so the only part that
// should be here.
//
// **The seam was extended, not rewritten**, which is the claim v0.4 made when it fixed
// `IFaceRenderer` against a stub: *"an interface agreed against a stub is one later versions
// extend; an interface discovered while writing the real thing is one everything else has to be
// rewritten around."* v2.2 added one overload -- `show(const EmotionFrame&)` -- and changed
// nothing else. The four methods agreed in v0.4 are still the four methods.
//
// **Everything composites into one full-screen sprite and is pushed in a single operation.** The
// stub established this and it matters more now, not less: drawing primitives straight to the panel
// makes the eyes appear before the mouth, so a blink becomes a glitch. The sprite is why the DoD
// can say "no tearing" and mean it, and why the chrome can be drawn into the same buffer rather
// than racing the face for the panel.

#pragma once

#include <M5Unified.h>

#include "app/face_renderer.h"
#include "pure/crossfade.h"
#include "pure/face.h"
#include "pure/facehold.h"
#include "pure/idle.h"
#include "pure/layers.h"
#include "pure/layout.h"
#include "pure/lipsync.h"
#include "pure/state.h"

namespace app {

class ProceduralRenderer : public IFaceRenderer {
  public:
    bool begin() override;
    void show(roboface::DeviceState state) override;
    void show(const roboface::EmotionFrame& frame) override;
    void setAudioLevel(float level) override;
    void tick(uint32_t now_ms) override;

    //: Whether the device's own speaker is running. **This, not the frame's `speaking` flag, is
    //: what keeps the mouth moving**: the server is seconds ahead of the speaker because the
    //: device is still draining audio the server finished sending, and `v2.1.2` was exactly the
    //: bug of believing the server about it. The flag is a permission; this is the fact.
    void setPlaying(bool playing);

    //: The chrome draws into the same sprite, so face and chrome are pushed together and cannot
    //: tear relative to each other.
    M5Canvas* canvas() { return ready_ ? &sprite_ : nullptr; }

    //: Push the composed sprite to the panel. The caller owns the cadence -- RF-058 needs to be
    //: able to skip a frame, and a renderer that pushed on its own schedule could not be asked to.
    void push();

    //: Whether the face has moved since the last push. The idle loop and a running crossfade both
    //: make this true; a settled face makes it false, and a settled face costs nothing.
    bool isAnimating() const { return animating_; }

    //: Frames actually pushed, for the budget RF-058 measures.
    uint32_t framesPushed() const { return frames_pushed_; }

    //: What the mouth did since this was last called, and the levels that drove it. **A diagnostic,
    //: and deliberately a cheap one**: three numbers accumulated in `tick` and read by the status
    //: line every ten seconds. The tempting version -- printing each shape change as it happens --
    //: is the mistake this project has now made three times: a diagnostic that starves the thing it
    //: diagnoses. Serial at 115200 costs about 1 ms a line, and the mouth changes several times a
    //: second while the same loop is feeding the speaker.
    struct MouthStats {
        uint32_t changes;    // shape transitions
        float level_min;     // the quietest level seen while speaking
        float level_max;     // the loudest
    };
    MouthStats takeMouthStats() {
        const MouthStats stats{mouth_changes_, level_seen_ ? level_min_ : 0.0f, level_max_};
        mouth_changes_ = 0;
        level_seen_ = false;
        level_min_ = 1.0f;
        level_max_ = 0.0f;
        return stats;
    }

  private:
    //: Put a frame into force. Separate from `show` because a frame that ends the speaking is
    //: **held** until the speaker stops -- see the note there.
    void apply(const roboface::EmotionFrame& frame);

    void compose(const roboface::LayerBank& bank);
    static uint16_t dimmed(uint16_t colour, uint8_t brightness);

    M5Canvas sprite_{&M5.Display};
    bool ready_ = false;

    roboface::Crossfade crossfade_{roboface::recipeFor(roboface::DeviceState::kBoot)};
    roboface::IdleLoop idle_;
    float audio_level_ = 0.0f;
    //: Whether the mouth should follow the audio, as decided by the **device's own state machine**.
    //:
    //: **This looks like dead code from v2.2 onward, and is not** (v2.2 review #6, closed in v2.3).
    //: `show(DeviceState)` is now called only for device-owned states, so nothing sets this to true
    //: any more -- *unless the server never sends `emotion{}` at all*, in which case `server_face`
    //: stays false, the device keeps choosing its own expression, and this is the path the mouth
    //: takes.
    //:
    //: That is not hypothetical. RF-063 spent an afternoon on it: the board was talking to a
    //: pre-v2.2 server on the same LAN, and this fallback is what kept the mouth working while the
    //: emotion channel appeared to be broken. Deleting it as unreachable would remove the only
    //: thing that makes an older server degrade gracefully rather than silently.
    bool speaking_mouth_ = false;
    //: The server's permission, and the device's own fact. The mouth runs on **both**: the server
    //: says a reply is being spoken, the speaker says it still is.
    bool speaking_allowed_ = false;
    bool playing_ = false;
    //: How long the server's last frame stands. `pure/facehold.h` owns the rule; this holds the
    //: state. Zero means no server frame is in force -- which is what a device-local face leaves
    //: behind, because `boot` and `offline` are not things that expire.
    roboface::FaceHold hold_;
    //: A frame that arrived while the device was still speaking and ends the speaking. Applied
    //: when playback stops, which is the moment it was actually describing.
    roboface::EmotionFrame pending_;
    bool has_pending_ = false;
    roboface::LipSync lips_;
    roboface::MouthFrame last_mouth_ = roboface::MouthFrame::kClosed;
    float last_mouth_open_ = 0.0f;
    float last_mouth_width_ = 1.0f;

    uint32_t mouth_changes_ = 0;
    bool level_seen_ = false;
    float level_min_ = 1.0f;
    float level_max_ = 0.0f;

    uint32_t last_tick_ms_ = 0;
    bool animating_ = true;
    uint32_t frames_pushed_ = 0;

    //: The bank drawn last, so an unchanged face can skip the whole compose-and-push.
    roboface::FaceRecipe last_drawn_{};
    bool has_drawn_ = false;
};

}  // namespace app

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
#include "pure/skins.h"
#include "pure/layout.h"
#include "pure/lipsync.h"
#include "pure/reflex.h"
#include "pure/state.h"

namespace app {

class ProceduralRenderer : public IFaceRenderer {
  public:
    bool begin() override;
    void show(roboface::DeviceState state) override;
    void show(const roboface::EmotionFrame& frame) override;
    void setAudioLevel(float level) override;
    void tick(uint32_t now_ms) override;

    //: Pull the gaze toward something, or release it. **A local reflex, and it outranks the
    //: server's `gaze`** -- ARCHITECTURE §Gaze says so: the field arrives from the server and is
    //: "overridden locally by a reflex". The device can see a hand; the server cannot.
    void setGazeReflex(bool active, float x = 0.0f);

    //: Where a voice is (v2.5), or that there is no opinion. **Below a reflex and above the
    //: server** -- ARCHITECTURE §Gaze: a hand reaching toward the device is a nearer and more
    //: specific claim than someone talking across the room, and the local estimate beats the
    //: server's because it is the same fact without the round trip.
    void setGazeVoice(bool present, float x = 0.0f);

    //: Wear a different face. **The renderer holds no face of its own** (v2.6): stackchan is a
    //: manifest like the four spirits, which is what makes "adding a skin requires no renderer code
    //: change" a property rather than an intention.
    void setSkin(const roboface::Skin& skin);

    //: Send only part of the sprite to the panel.
    //:
    //: **The whole sprite is 153 KB and takes 32 ms over SPI**, measured, every time — and while a
    //: reply plays, the only thing that changes is a mouth. Thirty-two milliseconds is half the
    //: speaker's entire buffer spent redrawing pixels that are already correct.
    //:
    //: The clip is set on the *destination*, which is what makes this cheap: the transfer is
    //: clipped before it happens rather than after.
    void pushRegion(int x, int y, int w, int h);

    //: Whether anything outside the mouth has changed since the last full push. The loop asks, so
    //: that a partial push is taken only when it is certainly enough.
    bool onlyMouthChanged() const { return !dirty_all_; }
    void markDirty() { dirty_all_ = true; }

  private:
    //: The silhouette and the element, which are the renderer's **vocabulary** rather than any
    //: skin's content.
    //:
    //: This is the honest boundary of *"adding a skin requires no renderer code change"*, and it is
    //: worth stating rather than discovering: a new skin that combines an existing body, an existing
    //: element, its own anchors and its own palette costs **nothing** here. A skin that needs a
    //: shape nobody has drawn before costs one `case`. `SkinBody` and `SkinElement` are closed enums
    //: precisely so that this vocabulary is finite, known, and small -- and so that a skin needing a
    //: sixth entry is announcing that the schema is too narrow, loudly, at the moment it is written.
    void drawBody(const roboface::LayerBank& bank);
    void drawElement(const roboface::LayerBank& bank);

  public:
    const roboface::Skin& skin() const { return skin_; }

    //: Fire a reflex over the current expression. The renderer owns the layer because the layer's
    //: output is a recipe and the renderer is the only thing that draws one.
    void fireReflex(roboface::Reflex reflex, uint32_t now_ms) { reflexes_.fire(reflex, now_ms); }
    void recordTap(uint32_t now_ms) { reflexes_.tapped(now_ms); }

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
    //: The server's gaze, from the last `EmotionFrame`, and the local reflex that may override it.
    float server_gaze_x_ = 0.0f;
    bool reflex_gaze_ = false;
    float reflex_gaze_x_ = 0.0f;
    bool voice_gaze_ = false;
    float voice_gaze_x_ = 0.0f;

    //: The face currently worn. Defaulted to the procedural one so a renderer that is never told
    //: anything still draws something -- RF-083's fallback is the same idea, arrived at from the
    //: other direction.
    roboface::Skin skin_ = roboface::stackchan();

    //: Which emotion the face is currently wearing. The crossfade holds the *recipe* -- eye
    //: openness, brow angle, mouth curve -- which is what a face looks like, and deliberately not
    //: which emotion produced it, because two emotions can share a recipe. The elements need the
    //: emotion itself: a flame is blue when sad, and no amount of brow angle says so.
    roboface::Emotion emotion_ = roboface::Emotion::kNeutral;

    //: Set by everything that repaints more than a mouth: a new expression, a state change, a
    //: blink, the idle drift moving, a skin change. Cleared by a full push. **Defaults to true**,
    //: because the safe answer to "has anything else changed" before anything has been drawn is
    //: yes -- a partial push of a screen that was never fully drawn shows a mouth on noise.
    bool dirty_all_ = true;
    //: A frame that arrived while the device was still speaking and ends the speaking. Applied
    //: when playback stops, which is the moment it was actually describing.
    roboface::EmotionFrame pending_;
    bool has_pending_ = false;
    roboface::ReflexLayer reflexes_;
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

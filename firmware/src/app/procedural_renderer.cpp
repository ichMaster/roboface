#include "app/procedural_renderer.h"

namespace app {
namespace {

using roboface::kScreenHeight;
using roboface::kScreenWidth;

//: The palette. One accent for every feature: this is a character, not a diagram, and a face whose
//: parts are different colours reads as a control panel. v2.6's skins replace these three values
//: and nothing else -- which is what "a skin is an asset swap" has to mean in practice.
// The face's colours moved to `pure/skins.h` in v2.6 -- they are the stackchan manifest's now, and
// the renderer reads whichever manifest it is wearing. Three constants here were the last thing
// that made one face special.

//: How far the mouth's curve is drawn as line segments. Twelve is smooth at this size and cheap;
//: a real quadratic per pixel would be the frame budget spent on something nobody can see.
constexpr int kMouthSegments = 12;

//: Below this, a change in a recipe field cannot move a feature by a whole pixel, so redrawing
//: would produce identical bytes at the cost of a push. Chosen from the geometry rather than by
//: feel: the mouth travels 22 px over a range of 1.0, so 1/44 is half a pixel.
constexpr float kVisibleChange = 0.02f;

//: Did anything actually change enough to see? Field by field, because a *distance* over five
//: dimensions lets a large move in one be cancelled by a small one in another.
bool changedVisibly(const roboface::FaceRecipe& a, const roboface::FaceRecipe& b) {
    const auto differs = [](float x, float y) {
        const float delta = x > y ? x - y : y - x;
        return delta >= kVisibleChange;
    };
    return differs(a.eye_openness, b.eye_openness) || differs(a.mouth_curve, b.mouth_curve) ||
           differs(a.brow_angle, b.brow_angle) || differs(a.tilt, b.tilt) || differs(a.dim, b.dim);
}

}  // namespace

bool ProceduralRenderer::begin() {
    M5.Display.setRotation(1);

    // **PSRAM, explicitly.** A 320x240 16-bit sprite is 150 KB. Internal RAM is where the I2S DMA
    // buffers come from, and v1 spent an evening on precisely that collision: with internal RAM
    // exhausted the microphone reported a successful start and captured nothing. PSRAM only began
    // working in v1.4.2 -- before that this allocation would have taken half of what was left.
    sprite_.setPsram(true);

    // **16 bits, and this reverses a decision that was right when it was made.**
    //
    // Eight bits was chosen for the frame budget and measured: a 16-bit push cost ~48 ms of blocked
    // loop against ~32 ms for eight, and at the time the face was a background, a glow and one ink
    // -- a 256-entry palette was more than it could use. That reasoning was sound and it expired
    // when the faces stopped being three colours.
    //
    // Drawn art cannot live in a palette. Not because 256 colours is too few for one body, but
    // because **an index cannot be blended**: the features are alpha masks tinted in the skin's ink
    // and the overlays are semi-transparent, and every one of those composites needs to mix two
    // colours per pixel. In palette space there is no such operation.
    //
    // The cost is paid back by the partial push (v2.6.2): while a reply plays, only the mouth band
    // is sent, which is 24,000 pixels rather than 76,800. A full push is now ~48 ms and happens on
    // an expression change rather than on every syllable.
    sprite_.setColorDepth(16);
    ready_ = sprite_.createSprite(kScreenWidth, kScreenHeight) != nullptr;
    if (!ready_) return false;  // say so rather than presenting a blank screen as a working one

    crossfade_.setResting(roboface::recipeFor(roboface::DeviceState::kIdle));
    sprite_.fillSprite(skin_.background);
    push();
    return true;
}

void ProceduralRenderer::show(roboface::DeviceState state) {
    // **The idle loop exists for silence.** Breathing, drifting and blinking are how a face says
    // "someone is here" when nothing else is happening. During a turn something else already says
    // it, and the idle then costs frames without adding anything a person notices.
    //
    // Listening: **completely still.** The level meter in the bottom band is already moving with
    // the person's own voice, and it is a truer signal of attention than a breath -- it responds to
    // *them*. A face that also drifts and blinks over it is two animations competing to say the
    // same thing, and the one that costs the microphone frames is the one that says it worse.
    //
    // Replying: the drift stops too, but for a different reason. What moves while the device speaks
    // is the **mouth**, following the reply's own loudness. A breath underneath that is noise on
    // the signal -- and the mouth is the thing a person is actually watching.
    //
    // Everything else -- idle, connecting, offline -- keeps the full loop, which is exactly where
    // it belongs: those are the states where nothing else is moving.
    const bool in_a_turn = state == roboface::DeviceState::kListening ||
                           state == roboface::DeviceState::kReplying;
    idle_.setIntensity(in_a_turn ? 0.0f : 1.0f);
    idle_.setBlinking(state != roboface::DeviceState::kListening);
    speaking_mouth_ = state == roboface::DeviceState::kReplying;
    if (!speaking_mouth_) lips_.reset();  // the mouth shuts when the reply ends

    // **The device has taken its face back, so nothing the server said is still in force**
    // (code review #2). Without this the last frame's ttl keeps running underneath: a link dropped
    // mid-reply shows the `offline` face, and up to sixty seconds later that face silently
    // crossfades to `neutral` and starts blinking over a device that is still offline -- with the
    // chrome's fault indicator arguing with it. The window is the length of whatever ttl happened
    // to be standing, so the symptom is intermittent by construction.
    //
    // A held instruction goes with it: it came from a connection that is no longer there.
    hold_.release();
    has_pending_ = false;
    // Sets a target; the drawing happens in `tick`. A `show` that drew would make a state change
    // cost a frame at the moment the device is busiest -- which is exactly when states change.
    crossfade_.target(roboface::recipeFor(state));
    animating_ = true;
}

void ProceduralRenderer::show(const roboface::EmotionFrame& frame) {
    // **The device renders what it is given.** This method is the whole of v2.2 on the device
    // side: no rule here decides what the face means, only how long the instruction stands and
    // whether the mouth is allowed to move.
    //
    // **A frame that ends the speaking is about a moment that has not arrived yet.**
    //
    // The server sends `emotion{neutral, speaking: false}` when *it* has finished the turn. The
    // device is seconds behind that -- 244 KB of buffered audio, about eight seconds of voice --
    // so applying it on arrival shuts the mouth and restarts the breathing drift in the middle of a
    // sentence the device is still saying. That is exactly the pair of faults `v2.1.2` fixed,
    // arriving through the server channel instead of the device's own state machine (code review
    // #1), and the pure `mouthRuns` test does not catch it because the rule is right and what feeds
    // the rule was wrong.
    //
    // ARCHITECTURE §EmotionFrame already says which half decides: *"`speaking` is a permission, not
    // a duration … what stops the mouth is the device's own playback state."* So the instruction is
    // **held** and applied when the speaker actually stops. A frame that grants permission, or any
    // frame arriving with nothing playing, applies at once.
    if (!roboface::appliesNow(frame.speaking, playing_)) {
        pending_ = frame;
        has_pending_ = true;
        return;
    }
    apply(frame);
}

void ProceduralRenderer::apply(const roboface::EmotionFrame& frame) {
    // The idle loop runs unless a reply is being spoken. `listening` is no longer special-cased by
    // state, because there is no state to case on -- what arrives is `calm`, and a calm face that
    // breathes is right. What must not breathe is a face that is talking, where the mouth is
    // already the thing being watched.
    idle_.setIntensity(frame.speaking ? 0.0f : 1.0f);
    idle_.setBlinking(!frame.speaking);

    speaking_allowed_ = frame.speaking;
    if (!frame.speaking) lips_.reset();

    // The ttl is restarted by every frame, which is why it never expires in normal operation: the
    // server sends one on each state change. What it bounds is the connection dying between two of
    // them, leaving a `thinking` face standing over a turn that will never finish.
    hold_.hold(frame.ttl_ms);
    // Remembered rather than applied: a reflex may be overriding it, and when the reflex releases
    // the face should return to what the server last asked for rather than to centre.
    server_gaze_x_ = frame.has_gaze ? frame.gaze_x : 0.0f;

    if (frame.emotion != emotion_) body_dirty_ = true;
    emotion_ = frame.emotion;
    dirty_all_ = true;
    crossfade_.target(roboface::withIntensity(roboface::recipeFor(frame.emotion), frame.intensity));
    animating_ = true;
}

void ProceduralRenderer::setGazeReflex(bool active, float x) {
    // **The local reflex wins while it is active**, which ARCHITECTURE §Gaze specifies and this is
    // the first code to depend on: the field has been parsed since v2.2 and ignored until now.
    //
    // The reason is not politeness about layering. The device can see a hand approaching and the
    // server cannot -- it is one round trip and a model call away from knowing, by which time the
    // hand has arrived. A gaze that waited for permission would be a gaze that always looked at
    // where the hand had been.
    reflex_gaze_ = active;
    reflex_gaze_x_ = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    dirty_all_ = true;
    animating_ = true;
}

void ProceduralRenderer::setSkin(const roboface::Skin& skin) {
    // **Refused rather than drawn** if it cannot be drawn. A manifest whose eyes fall outside the
    // face area does not crash -- it paints over the battery indicator, and someone notices weeks
    // later. Keeping the current face is always safe; wearing a broken one never is.
    if (roboface::validate(skin) != roboface::SkinFault::kNone) return;
    skin_ = skin;
    body_dirty_ = true;
    dirty_all_ = true;
    animating_ = true;
}

void ProceduralRenderer::setGazeVoice(bool present, float x) {
    // **Below a reflex, above the server.** A hand reaching toward the device is a nearer and more
    // specific claim on attention than a voice across the room; and this beats the server's `gaze`
    // because it is the same fact without the round trip -- the frame that comes back is a
    // confirmation of something the face already did.
    const float clamped = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    if (present == voice_gaze_ && clamped == voice_gaze_x_) return;
    voice_gaze_ = present;
    voice_gaze_x_ = clamped;
    dirty_all_ = true;
    animating_ = true;
}

void ProceduralRenderer::setPlaying(bool playing) {
    const bool was_playing = playing_;
    playing_ = playing;
    if (playing || !was_playing) return;

    // The speaker has stopped, which is the moment a held instruction was waiting for. The mouth
    // shuts here -- not when the server said the reply was over, which it did seconds earlier.
    if (has_pending_) {
        has_pending_ = false;
        apply(pending_);
        return;
    }
    // Nothing held: playback ended without the server saying so -- a drained buffer after a dropped
    // link, or a `/loopback` recording. The mouth still shuts.
    lips_.reset();
    speaking_allowed_ = false;
}

void ProceduralRenderer::setAudioLevel(float level) {
    // The mouth's signal. This method was declared in v0.4 against a stub and did nothing until
    // now, which is exactly what the seam was fixed early for.
    audio_level_ = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

void ProceduralRenderer::tick(uint32_t now_ms) {
    if (!ready_) return;

    const uint32_t delta_ms = last_tick_ms_ == 0 ? 0 : now_ms - last_tick_ms_;
    last_tick_ms_ = now_ms;
    if (delta_ms == 0) return;

    // Expiry before anything is drawn. A frame that has outlived its ttl is an instruction from a
    // server that has stopped talking, and holding it would leave the device wearing an expression
    // about a turn that ended -- or never ended, which is worse and is what the ttl is for.
    if (hold_.advance(delta_ms)) {
        crossfade_.target(roboface::recipeFor(roboface::Emotion::kNeutral));
        // The idle loop comes back with it: a face that relaxed to neutral and then sat perfectly
        // still would read as frozen rather than resting, which is a worse thing to look at than
        // the stale expression this just replaced.
        idle_.setIntensity(1.0f);
        idle_.setBlinking(true);
        speaking_allowed_ = false;
    }

    const roboface::FaceRecipe expression = crossfade_.advance(delta_ms);
    const roboface::IdleOffsets idle = idle_.advance(delta_ms);

    // The idle loop modulates the expression rather than replacing it: a blink during a smile is
    // still a smile. Multiplying openness is what makes that true for every recipe at once.
    roboface::FaceRecipe frame = expression;
    frame.eye_openness *= idle.eye_scale;

    // **The reflex composites over everything else and touches nothing underneath.** It is applied
    // after the crossfade and the idle loop, so a poke during a smile is a poked smile; and it is
    // told whether the device is speaking, because while it is the mouth belongs to the lip-sync
    // and two animations on one feature is one too many.
    frame = reflexes_.apply(frame, now_ms, speaking_allowed_ || playing_);
    if (reflexes_.isActive(now_ms)) animating_ = true;

    // **A simplified lip-sync: four mouth shapes, not a continuous opening.** v2.3 does the real
    // thing -- visemes chosen from the spectrum rather than the amplitude. This is the animator's
    // version, and it is better than a smooth mouth on both counts that matter here: real speech
    // moves between a few positions rather than sliding, and a shape only redraws when it *changes*,
    // where a continuous mouth redraws on every frame.
    //
    // The shape sets how far the mouth **opens**, which is separate from how it is curved -- so the
    // device still smiles while it talks. Driving this through `mouth_curve` was a real mistake and
    // not a hypothetical one: a face already smiling at 0.70 hit the ceiling on the first shape and
    // moved two pixels, so the lip-sync ran perfectly and was invisible.
    // **Both, and this is the v2.1.2 rule expressed in one line.** The server's permission says a
    // reply is being spoken; the device's own speaker says it still is. Believing only the server
    // froze the mouth mid-reply, because the server is seconds ahead of the audio it already sent.
    // Believing only the speaker would move the mouth for a loopback test or a notification chime.
    const bool mouth_runs = roboface::mouthRuns(speaking_allowed_ || speaking_mouth_, playing_);

    roboface::MouthPose mouth;
    if (mouth_runs) {
        const roboface::MouthFrame shape = lips_.feed(audio_level_);
        mouth = lips_.pose();
        if (shape != last_mouth_) {
            last_mouth_ = shape;
            ++mouth_changes_;
        }
        if (!level_seen_ || audio_level_ < level_min_) level_min_ = audio_level_;
        if (audio_level_ > level_max_) level_max_ = audio_level_;
        level_seen_ = true;
    }

    // **The manifest's geometry, not a default-constructed one.** This was the line that made the
    // renderer own a face: every skin's anchors arrive here now.
    roboface::FaceGeometry geometry = skin_.geometry;
    geometry.centre_y += static_cast<int>(idle.bob_y);
    geometry.eye_offset_y += static_cast<int>(idle.gaze_y);
    geometry.centre_x += static_cast<int>(idle.gaze_x);

    // The gaze, in the documented order of authority (ARCHITECTURE §Gaze): a local reflex, then
    // the voice direction, then whatever the server last asked for. The idle drift above is the
    // fourth and smallest -- a wander, not a look, and it composes with any of them.
    //
    // **Absence is what makes the order work.** Each source is skipped when it has no opinion
    // rather than contributing a zero, so a speaker sitting directly in front of the device leaves
    // the face to its drift instead of pinning the eyes forward.
    const float gaze_x = reflex_gaze_  ? reflex_gaze_x_
                         : voice_gaze_ ? voice_gaze_x_
                                       : server_gaze_x_;
    geometry.centre_x +=
        static_cast<int>(gaze_x * static_cast<float>(skin_.gaze_travel_px));

    // Skip the compose when nothing moved. **Narrower than it sounds**: the breath is a continuous
    // wave, so this only fires when the idle is *stilled* -- `intensity` at zero, which is what
    // v2.2 does during a turn -- and no fade is running. Said plainly because the tempting version
    // of this comment ("a settled face costs nothing") would send the next person chasing a frame
    // budget straight past the renderer.
    // `areDistinct` is deliberately *not* used here. It answers "is this change worth animating",
    // with a threshold coarse enough for that -- and the mouth moving to a syllable is far below it,
    // so a face that talked would have sat perfectly still. This is the finer question: did any
    // number change at all.
    const bool moved = !has_drawn_ || crossfade_.isFading() || idle_.isBlinking() ||
                       changedVisibly(frame, last_drawn_) ||
                       mouth.open != last_mouth_open_ || mouth.width != last_mouth_width_ ||
                       idle.bob_y != 0.0f || idle.gaze_x != 0.0f;

    // **Everything except the mouth moving is a reason to repaint the whole screen.** Stated as the
    // complement rather than as a list of mouth cases: a new item in the recipe, a new idle motion,
    // a new layer would each otherwise default to "a partial push is fine" and be found later as a
    // face that half-updated.
    if (!has_drawn_ || crossfade_.isFading() || idle_.isBlinking() ||
        changedVisibly(frame, last_drawn_) || idle.bob_y != 0.0f || idle.gaze_x != 0.0f) {
        dirty_all_ = true;
    }
    animating_ = moved;
    if (!moved) return;

    compose(roboface::layout(frame, geometry, mouth.open, mouth.width));
    last_drawn_ = frame;
    last_mouth_open_ = mouth.open;
    last_mouth_width_ = mouth.width;
    has_drawn_ = true;
}


void ProceduralRenderer::drawElement(const roboface::LayerBank& bank) {
    // **The overlays keep their own colours** -- a blush is pink whatever the skin's ink is, and a
    // tear is blue. That is the one place in this pipeline where the artist's palette reaches the
    // panel unmediated, and it is why these are RGB565 rather than alpha masks.
    //
    // The coordinates are the manifest's, returned with the art. They are spelled here rather than
    // derived because they are placements on a specific drawing: where a cheek is on *this* ghost.
    uint16_t* canvas = static_cast<uint16_t*>(sprite_.getBuffer());
    if (canvas == nullptr) return;
    (void)bank;

    switch (skin_.element) {
        case roboface::SkinElement::kBlushAndTear:
            if (emotion_ == roboface::Emotion::kJoy || emotion_ == roboface::Emotion::kNeutral ||
                emotion_ == roboface::Emotion::kSurprised) {
                art::drawOverlay(canvas, assets::kElemGhostBlush, 76, 146);
            }
            if (emotion_ == roboface::Emotion::kSad) {
                art::drawOverlay(canvas, assets::kElemGhostTear, 214, 148);
            }
            return;

        case roboface::SkinElement::kGlowFollowsEmotion:
            // Always, on this skin: the bell glows whatever the mood, and the mood is the colour
            // the body was already tinted with.
            art::drawOverlay(canvas, assets::kElemJellyGlow, 60, 40);
            return;

        case roboface::SkinElement::kWeatherFollowsEmotion:
            if (emotion_ == roboface::Emotion::kJoy) {
                art::drawOverlay(canvas, assets::kElemCloudSun, 228, 34);
            }
            if (emotion_ == roboface::Emotion::kSad) {
                art::drawOverlay(canvas, assets::kElemCloudRain, 60, 168);
            }
            return;

        case roboface::SkinElement::kPaletteFollowsEmotion:
            // The flame's palette *is* its body, tinted already. Nothing on top.
            return;

        case roboface::SkinElement::kNone:
        case roboface::SkinElement::kCount:
            return;
    }
}

void ProceduralRenderer::compose(const roboface::LayerBank& bank) {
    // **The face area only.** The outer 28 px bands belong to chrome (DEVICE_UI §Layout), and a
    // full-sprite clear would wipe the link, the battery and the microphone button within 55 ms of
    // each event and leave them gone until the next one -- which reads as the chrome flickering,
    // and gets looked for in the chrome.
    uint16_t* canvas = static_cast<uint16_t*>(sprite_.getBuffer());
    if (canvas == nullptr) return;

    const std::size_t skin_index = roboface::skinIndexFor(skin_.name);
    const bool tinted = skin_.element == roboface::SkinElement::kPaletteFollowsEmotion ||
                        skin_.element == roboface::SkinElement::kGlowFollowsEmotion ||
                        skin_.element == roboface::SkinElement::kWeatherFollowsEmotion;
    const uint16_t tint = skin_.element_palette.at(emotion_);
    const uint16_t ink = dimmed(skin_.ink, bank.brightness);

    // The gaze moves the features across the body, which is the whole reason they are separate
    // images: the character looks at you, and the costume does not follow.
    const float gaze_x = reflex_gaze_  ? reflex_gaze_x_
                         : voice_gaze_ ? voice_gaze_x_
                                       : server_gaze_x_;
    const int shift = static_cast<int>(gaze_x * static_cast<float>(skin_.gaze_travel_px));

    // **The whole body, or only the ground the features stand on.** After the first frame the body
    // has not changed and the features have, so restoring 23,000 pixels beats redrawing 76,800 --
    // and this loop runs while a reply is playing, where every millisecond is one the speaker does
    // not get.
    if (body_dirty_) {
        art::drawBodyRegion(canvas, skin_index, tinted, tint, 0, 0, roboface::kScreenWidth,
                            roboface::kScreenHeight);
        body_dirty_ = false;
    } else {
        art::drawBodyRegion(canvas, skin_index, tinted, tint, art::kEyesX - skin_.gaze_travel_px,
                            art::kEyesY, assets::kEyesNeutral.width + 2 * skin_.gaze_travel_px,
                            assets::kEyesNeutral.height);
        art::drawBodyRegion(canvas, skin_index, tinted, tint, art::kMouthX - skin_.gaze_travel_px,
                            art::kMouthY, assets::kMouthNeutral.width + 2 * skin_.gaze_travel_px,
                            assets::kMouthNeutral.height);
    }

    // Eyes. A blink replaces the image outright rather than scaling one: the artist drew a closed
    // eye, and squashing an open one is how a face ends up looking like a puppet.
    const bool blinking = bank.left_eye.half_height <= 1;
    art::drawMask(canvas, blinking ? assets::kEyesClosed : art::eyesFor(emotion_),
                  art::kEyesX + shift, art::kEyesY, ink);

    // The mouth, and **the fact rather than the permission**: `playing_` is whether this device's
    // speaker is running, which is what v2.1.2 established the mouth must follow. The server's
    // idea of when a reply ends runs seconds ahead of the audio still draining.
    if (playing_) {
        art::drawMask(canvas, art::visemeFor(last_mouth_), art::kMouthX + shift, art::kMouthY, ink);
    } else {
        art::drawMask(canvas, art::mouthFor(emotion_), art::kMouthX + shift, art::kMouthY, ink);
    }

    drawElement(bank);
}

uint16_t ProceduralRenderer::dimmed(uint16_t colour, uint8_t brightness) {
    if (brightness >= 255) return colour;
    // RGB565, scaled per channel. Scaling the packed value would bleed red into green.
    const uint32_t r = ((colour >> 11) & 0x1F) * brightness / 255;
    const uint32_t g = ((colour >> 5) & 0x3F) * brightness / 255;
    const uint32_t b = (colour & 0x1F) * brightness / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void ProceduralRenderer::push() {
    if (!ready_) return;
    sprite_.pushSprite(0, 0);
    dirty_all_ = false;
    ++frames_pushed_;
}

void ProceduralRenderer::pushRegion(int x, int y, int w, int h) {
    if (!ready_) return;
    M5.Display.setClipRect(x, y, w, h);
    sprite_.pushSprite(0, 0);
    M5.Display.clearClipRect();
    ++frames_pushed_;
}

}  // namespace app

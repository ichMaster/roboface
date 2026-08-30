#include "app/procedural_renderer.h"

namespace app {
namespace {

using roboface::kScreenHeight;
using roboface::kScreenWidth;

//: The palette. One accent for every feature: this is a character, not a diagram, and a face whose
//: parts are different colours reads as a control panel. v2.6's skins replace these three values
//: and nothing else -- which is what "a skin is an asset swap" has to mean in practice.
constexpr uint16_t kBackground = 0x0000;  // black; the panel's own dark is the room
constexpr uint16_t kGlow = 0x0104;        // a very dark blue wash behind the head
constexpr uint16_t kInk = 0x5DFF;         // the features: a soft cyan-white

//: How far the mouth's curve is drawn as line segments. Twelve is smooth at this size and cheap;
//: a real quadratic per pixel would be the frame budget spent on something nobody can see.
constexpr int kMouthSegments = 12;

}  // namespace

bool ProceduralRenderer::begin() {
    M5.Display.setRotation(1);

    // **PSRAM, explicitly.** A 320x240 16-bit sprite is 150 KB. Internal RAM is where the I2S DMA
    // buffers come from, and v1 spent an evening on precisely that collision: with internal RAM
    // exhausted the microphone reported a successful start and captured nothing. PSRAM only began
    // working in v1.4.2 -- before that this allocation would have taken half of what was left.
    sprite_.setPsram(true);

    // **8 bits, not 16, and this is the frame budget rather than a preference.**
    //
    // Measured: each push of a 320x240 16-bit sprite costs about 48 ms of blocked loop -- 150 KB
    // over SPI -- which works out at roughly 2.4 lost microphone frames per drawn frame. At 7 FPS
    // the recorder ran at 35 frames a second instead of 50. Lowering the frame rate only trades one
    // for the other; halving the bytes moves the line.
    //
    // The face costs nothing in colour: a background, a glow and one ink. A 256-entry palette is
    // more than this renderer will ever need, and v2.6's skins are a palette swap by design.
    sprite_.setColorDepth(8);
    ready_ = sprite_.createSprite(kScreenWidth, kScreenHeight) != nullptr;
    if (!ready_) return false;  // say so rather than presenting a blank screen as a working one

    crossfade_.setResting(roboface::recipeFor(roboface::DeviceState::kIdle));
    sprite_.fillSprite(kBackground);
    push();
    return true;
}

void ProceduralRenderer::show(roboface::DeviceState state) {
    // **Listening has one settled face.** The drift is stilled while the device is paying
    // attention: a gaze wandering off while someone is mid-sentence reads as distraction, which is
    // the opposite of what the state means. Blinking stays -- a face that stops blinking stops
    // looking alive and starts looking frozen.
    //
    // It also buys back frames exactly when they are needed: with the drift stopped, the
    // unchanged-frame skip in `tick` finally fires, and the loop it frees is the loop the
    // microphone wants.
    idle_.setIntensity(state == roboface::DeviceState::kListening ? 0.0f : 1.0f);
    // Sets a target; the drawing happens in `tick`. A `show` that drew would make a state change
    // cost a frame at the moment the device is busiest -- which is exactly when states change.
    crossfade_.target(roboface::recipeFor(state));
    animating_ = true;
}

void ProceduralRenderer::setAudioLevel(float level) {
    // Held for v2.3's lip-sync. Recorded now rather than ignored, so the wiring is already right
    // when the mouth starts using it.
    audio_level_ = level;
}

void ProceduralRenderer::tick(uint32_t now_ms) {
    if (!ready_) return;

    const uint32_t delta_ms = last_tick_ms_ == 0 ? 0 : now_ms - last_tick_ms_;
    last_tick_ms_ = now_ms;
    if (delta_ms == 0) return;

    const roboface::FaceRecipe expression = crossfade_.advance(delta_ms);
    const roboface::IdleOffsets idle = idle_.advance(delta_ms);

    // The idle loop modulates the expression rather than replacing it: a blink during a smile is
    // still a smile. Multiplying openness is what makes that true for every recipe at once.
    roboface::FaceRecipe frame = expression;
    frame.eye_openness *= idle.eye_scale;

    roboface::FaceGeometry geometry;
    geometry.centre_y += static_cast<int>(idle.bob_y);
    geometry.eye_offset_y += static_cast<int>(idle.gaze_y);
    geometry.centre_x += static_cast<int>(idle.gaze_x);

    // Skip the compose when nothing moved. **Narrower than it sounds**: the breath is a continuous
    // wave, so this only fires when the idle is *stilled* -- `intensity` at zero, which is what
    // v2.2 does during a turn -- and no fade is running. Said plainly because the tempting version
    // of this comment ("a settled face costs nothing") would send the next person chasing a frame
    // budget straight past the renderer.
    const bool moved = !has_drawn_ || crossfade_.isFading() || idle_.isBlinking() ||
                       roboface::areDistinct(frame, last_drawn_) ||
                       idle.bob_y != 0.0f || idle.gaze_x != 0.0f;
    animating_ = moved;
    if (!moved) return;

    compose(roboface::layout(frame, geometry));
    last_drawn_ = frame;
    has_drawn_ = true;
}

void ProceduralRenderer::compose(const roboface::LayerBank& bank) {
    // **The face area only.** The outer 28 px bands belong to chrome (DEVICE_UI §Layout), and the
    // stub said so in its own docstring -- but the stub redrew only on an event, so clearing the
    // whole sprite cost nothing. This renderer redraws on its own schedule, ~18 times a second,
    // while `render()` still runs only when something happens: a full-sprite clear would wipe the
    // link, the battery and the muted-microphone indicator within 55 ms of each event and leave
    // them gone until the next one.
    //
    // Which would have been read as the chrome flickering, and looked for in the chrome.
    sprite_.fillRect(roboface::kFaceLeft, roboface::kFaceTop, roboface::kFaceWidth,
                     roboface::kFaceHeight, kBackground);

    const uint16_t glow = dimmed(kGlow, bank.brightness);
    const uint16_t ink = dimmed(kInk, bank.brightness);

    // In the order `Layer` declares, which is the order ARCHITECTURE names. Iterating an enum would
    // be tidier and would also mean a switch nobody reads; the order is visible here instead.
    sprite_.fillRoundRect(bank.glow.centre_x - bank.glow.half_width,
                          bank.glow.centre_y - bank.glow.half_height, bank.glow.half_width * 2,
                          bank.glow.half_height * 2, bank.glow.corner_radius, glow);

    // Eyes. A closed eye is a line, not a zero-height rectangle -- `fillRoundRect` with no height
    // draws nothing, and a blink that made the eyes vanish would look like a fault.
    for (const auto& eye : {bank.left_eye, bank.right_eye}) {
        if (eye.half_height <= 1) {
            sprite_.drawFastHLine(eye.centre_x - eye.half_width, eye.centre_y, eye.half_width * 2,
                                  ink);
        } else {
            sprite_.fillRoundRect(eye.centre_x - eye.half_width, eye.centre_y - eye.half_height,
                                  eye.half_width * 2, eye.half_height * 2,
                                  eye.half_width / 2, ink);
        }
    }

    // Brows, as thick lines between the two endpoints the layer bank worked out.
    for (const auto& brow : {bank.left_brow, bank.right_brow}) {
        for (int thickness = 0; thickness < 4; ++thickness) {
            sprite_.drawLine(brow.inner_x, brow.inner_y + thickness, brow.outer_x,
                             brow.outer_y + thickness, ink);
        }
    }

    // The mouth, as a quadratic through three control points, walked in segments.
    int previous_x = bank.mouth.left_x;
    int previous_y = bank.mouth.left_y;
    for (int step = 1; step <= kMouthSegments; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(kMouthSegments);
        const float inv = 1.0f - t;
        const float x = inv * inv * static_cast<float>(bank.mouth.left_x) +
                        2.0f * inv * t * static_cast<float>(bank.mouth.mid_x) +
                        t * t * static_cast<float>(bank.mouth.right_x);
        const float y = inv * inv * static_cast<float>(bank.mouth.left_y) +
                        2.0f * inv * t * static_cast<float>(bank.mouth.mid_y) +
                        t * t * static_cast<float>(bank.mouth.right_y);
        for (int thickness = 0; thickness < 3; ++thickness) {
            sprite_.drawLine(previous_x, previous_y + thickness, static_cast<int>(x),
                             static_cast<int>(y) + thickness, ink);
        }
        previous_x = static_cast<int>(x);
        previous_y = static_cast<int>(y);
    }
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
    ++frames_pushed_;
}

}  // namespace app

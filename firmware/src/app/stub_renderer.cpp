#include "app/stub_renderer.h"

#include <algorithm>
#include <cmath>

namespace app {
namespace {

// A muted teal on near-black: legible across a room without being a torch in a dark study, which
// is where a desk companion actually lives.
constexpr uint16_t kBackground = 0x0000;
constexpr uint16_t kFaceColour = 0x07FF;  // cyan-ish
constexpr uint16_t kBrowColour = 0x05DF;

// Geometry within the face safe area. Derived from it rather than from the screen, so the face
// cannot creep into chrome's band if the layout constants ever change.
constexpr int kEyeOffsetX = 62;   // from centre
constexpr int kEyeOffsetY = -22;  // above centre
constexpr int kEyeRadiusX = 30;
constexpr int kEyeMaxRadiusY = 34;
constexpr int kMouthOffsetY = 46;
constexpr int kMouthHalfWidth = 62;
constexpr int kMouthMaxRise = 34;
constexpr int kBrowOffsetY = -66;
constexpr int kBrowHalfWidth = 34;
constexpr int kBrowMaxTilt = 16;
constexpr int kMaxTiltPixels = 18;

// Scale a colour toward black. `dim` is 0..1 from the recipe; DEVICE_UI wants offline at ~60 %
// brightness, and doing it here rather than with a panel-brightness call keeps the dimming part of
// the *face* rather than of the device -- chrome beside it stays fully legible.
uint16_t dimmed(uint16_t colour565, float dim) {
    const float keep = std::clamp(1.0f - dim, 0.0f, 1.0f);
    const int r = static_cast<int>(((colour565 >> 11) & 0x1F) * keep);
    const int g = static_cast<int>(((colour565 >> 5) & 0x3F) * keep);
    const int b = static_cast<int>((colour565 & 0x1F) * keep);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

}  // namespace

bool StubRenderer::begin() {
    M5.Display.setRotation(1);

    // PSRAM, explicitly. A 320x240 16-bit sprite is 150 KB -- nearly half of the 320 KB of internal
    // RAM, which v1's audio buffers and v3's JPEG frames also need. The Core S3 has 8 MB of PSRAM
    // precisely for buffers like this one (ARCHITECTURE §Hardware map).
    sprite_.setPsram(true);
    sprite_.setColorDepth(16);
    ready_ = sprite_.createSprite(kScreenWidth, kScreenHeight) != nullptr;

    if (ready_) {
        sprite_.fillSprite(kBackground);
        push();
    }
    return ready_;
}

void StubRenderer::drawFace(const roboface::FaceRecipe& recipe) {
    const int centre_x = kScreenWidth / 2 + static_cast<int>(recipe.tilt * kMaxTiltPixels);
    const int centre_y = kScreenHeight / 2;

    const uint16_t face = dimmed(kFaceColour, recipe.dim);
    const uint16_t brow = dimmed(kBrowColour, recipe.dim);

    // Eyes: ellipses whose vertical radius follows openness. A closed eye is a line rather than a
    // zero-height ellipse, because zero-height draws nothing and the face would lose its eyes
    // entirely at `boot`.
    const int eye_ry = std::max(2, static_cast<int>(recipe.eye_openness * kEyeMaxRadiusY));
    for (const int side : {-1, 1}) {
        const int x = centre_x + side * kEyeOffsetX;
        const int y = centre_y + kEyeOffsetY;
        sprite_.fillEllipse(x, y, kEyeRadiusX, eye_ry, face);
    }

    // Brows: short lines whose inner ends rise or fall. Positive brow_angle is cross (inner ends
    // down), negative is worried (inner ends up) -- the difference between the error face and the
    // offline one, and the reason they do not read as the same unhappy face.
    const int brow_tilt = static_cast<int>(recipe.brow_angle * kBrowMaxTilt);
    for (const int side : {-1, 1}) {
        const int x = centre_x + side * kEyeOffsetX;
        const int y = centre_y + kBrowOffsetY;
        const int inner = x - side * kBrowHalfWidth;
        const int outer = x + side * kBrowHalfWidth;
        sprite_.drawWideLine(inner, y + brow_tilt, outer, y - brow_tilt / 2, 4, brow);
    }

    // Mouth: an arc approximated by a short polyline. `mouth_curve` lifts or drops the middle --
    // a smile is the ends below the centre, a frown the reverse.
    const int rise = static_cast<int>(recipe.mouth_curve * kMouthMaxRise);
    const int mouth_y = centre_y + kMouthOffsetY;
    int previous_x = centre_x - kMouthHalfWidth;
    int previous_y = mouth_y;
    constexpr int kSegments = 16;
    for (int i = 1; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / kSegments;          // 0..1
        const float parabola = 4.0f * t * (1.0f - t);               // 0 at ends, 1 in the middle
        const int x = centre_x - kMouthHalfWidth + static_cast<int>(t * 2 * kMouthHalfWidth);
        const int y = mouth_y - static_cast<int>(parabola * rise);
        sprite_.drawWideLine(previous_x, previous_y, x, y, 5, face);
        previous_x = x;
        previous_y = y;
    }
}

void StubRenderer::show(roboface::DeviceState state) {
    state_ = state;
    if (!ready_) return;

    // Clear only the face safe area. Chrome owns the outer band and redraws on its own schedule;
    // clearing the whole sprite here would erase it every time the state changed.
    sprite_.fillRect(kFaceLeft, kFaceTop, kFaceWidth, kFaceHeight, kBackground);
    drawFace(roboface::recipeFor(state));
}

void StubRenderer::setAudioLevel(float level) {
    // v1 drives the mouth from this. Stored rather than ignored so the plumbing is real before the
    // behaviour is.
    audio_level_ = std::clamp(level, 0.0f, 1.0f);
}

void StubRenderer::tick(uint32_t) {
    // Nothing to animate yet. v2.1 runs the idle loop, blinks and crossfades here.
}

void StubRenderer::push() {
    if (!ready_) return;
    // One operation, whole frame. This is the line that makes "no tearing" true.
    sprite_.pushSprite(0, 0);
}

}  // namespace app

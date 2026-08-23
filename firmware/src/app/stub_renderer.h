// The v0 face: a static drawing per device state, from RF-018's recipe.
//
// **Everything is drawn into a full-screen sprite in PSRAM and pushed in one operation.** Drawing
// primitives straight to the panel is what produces tearing -- the eyes appear before the mouth,
// and a state change looks like a glitch rather than an expression. The sprite is why the DoD can
// say "no tearing" and mean it, and why RF-021's chrome can be composited with the face rather
// than racing it.
//
// The face keeps the central 264x184 (DEVICE_UI §Layout). The outer 28 px bands belong to chrome
// and this class never draws into them.

#pragma once

#include <M5Unified.h>

#include "app/face_renderer.h"
#include "pure/face.h"
#include "pure/layout.h"
#include "pure/state.h"

namespace app {

// The geometry lives in pure/layout.h, where a host test proves the bands do not overlap.
using roboface::kFaceHeight;
using roboface::kFaceLeft;
using roboface::kFaceTop;
using roboface::kFaceWidth;
using roboface::kScreenHeight;
using roboface::kScreenWidth;

class StubRenderer : public IFaceRenderer {
  public:
    bool begin() override;
    void show(roboface::DeviceState state) override;
    void setAudioLevel(float level) override;
    void tick(uint32_t now_ms) override;

    // RF-021 draws chrome into the same sprite, so the two are pushed together and cannot tear
    // relative to each other.
    M5Canvas* canvas() { return ready_ ? &sprite_ : nullptr; }
    void push();

    bool ready() const { return ready_; }
    roboface::DeviceState currentState() const { return state_; }

  private:
    void drawFace(const roboface::FaceRecipe& recipe);

    M5Canvas sprite_{&M5.Display};
    bool ready_ = false;
    roboface::DeviceState state_ = roboface::DeviceState::kBoot;
    float audio_level_ = 0.0f;
};

}  // namespace app

// Draws what pure/chrome.h decides.
//
// This module owns pixels and no rules. *Whether* the link is shown, *whether* the battery matters
// and *what* the bottom band belongs to are all answered by `roboface::Chrome`, which is host-tested
// with a fake clock; this class asks and draws. That split is why there is not a single timing
// literal below -- the tested rules are the rules that run.
//
// It draws into the renderer's sprite, so chrome and face are pushed together and cannot tear
// relative to each other, and only ever into the outer 28 px bands (DEVICE_UI §Rules: never block
// the face). Every rectangle it touches is checked against `roboface::clearOfFace` first.

#pragma once

#include <M5Unified.h>

#include <cstddef>

#include "pure/carousel.h"
#include "pure/chrome.h"
#include "pure/layout.h"

namespace app {

class ChromeView {
  public:
    // `canvas` is the renderer's sprite. Null when the renderer failed to allocate, in which case
    // this draws nothing rather than crashing -- a device with no memory for a face should still
    // boot and say so on serial.
    //: `selected` and `skins` are the carousel's, read only when the band belongs to it. Passed
    //: rather than stored: this class owns pixels and no state, which is what lets `pure/chrome.h`
    //: remain the only thing that decides *whether* anything is shown.
    void draw(M5Canvas* canvas, const roboface::Chrome& chrome, float level = 0.0f,
              std::size_t selected = 0, std::size_t skins = 0);

  private:
    void drawLink(M5Canvas& canvas, roboface::LinkState state, uint8_t alpha);
    void drawMicButton(M5Canvas& canvas, bool muted);
    void drawBattery(M5Canvas& canvas, int percent, bool charging, uint8_t alpha);
    void drawFaultLine(M5Canvas& canvas, roboface::ErrorCode code);
    void drawLevelMeter(M5Canvas& canvas, float level);
    void drawCarousel(M5Canvas& canvas, std::size_t selected, std::size_t skins);
    void drawToast(M5Canvas& canvas, const char* text);
};

}  // namespace app

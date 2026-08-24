// The serial chat console's screen half: the conversation, drawn where the face usually is.
//
// Glue, because it is drawing. It composes exactly like `ChromeView` -- into the sprite
// `StubRenderer` owns, never straight to the panel -- so the transcript, the face area and chrome
// are pushed in one operation and cannot tear against each other.
//
// It keeps to the face's 264x184 safe area (DEVICE_UI §Layout). The outer bands belong to chrome,
// and a console that drew into them would hide the link and battery indicators at exactly the
// moment a dropped link is what you need to know about.

#pragma once

#include <M5Unified.h>

#include "pure/transcript.h"

namespace app {

class ConsoleView {
  public:
    // Draws the transcript into the face area. Safe with a null canvas -- the renderer returns one
    // only once its sprite has been allocated, and a failed allocation must not take the app down.
    void draw(M5Canvas* canvas, const roboface::Transcript& transcript) const;
};

}  // namespace app

// RoboFace firmware — entry point.
//
// v0.3 (RF-012) proves the toolchain and no more: it boots, brings the screen up and says what
// it is over serial. The state machine, WiFi, the WebSocket client and the serial debug channel
// arrive in RF-014 through RF-017; the face is v0.4's.
//
// This file is glue and stays glue: wiring only, no decisions. Everything that decides anything
// lives under `src/pure/` where a host can test it.

#include <M5Unified.h>

#include "pure/version.h"

void setup() {
    auto config = M5.config();
    M5.begin(config);

    Serial.begin(115200);
    // The USB CDC port takes a moment to enumerate after a reset; without this the first lines
    // are written into a port nobody is attached to yet, which looks exactly like a board that
    // failed to boot.
    delay(300);

    Serial.println();
    Serial.printf("RoboFace firmware %s on %s\n", roboface::kFirmwareVersion, roboface::kBoard);
    Serial.println("v0.3 scaffold: no WiFi, no socket, no face yet.");

    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.printf("RoboFace\n%s\n", roboface::kFirmwareVersion);
}

void loop() {
    M5.update();
    delay(10);
}

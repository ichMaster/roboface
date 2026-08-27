// The firmware's own identity, and the first thing in `namespace roboface`.
//
// Header-only, Arduino-free: this is the pattern every module under `pure/` follows
// (ARCHITECTURE.md §Firmware architecture). Nothing here may include <M5Unified.h>, <WiFi.h>
// or anything else that needs a board, which is why `pio test -e native` can compile it.

#pragma once

// Both environments must agree on the language standard, or the code the host tests prove is not
// the code the board runs. The ESP32 core appends its own -std=gnu++11; platformio.ini unsets it,
// and this is what notices if that ever stops working -- in the build, in whichever environment
// broke, rather than as a puzzling constexpr error three headers away.
static_assert(__cplusplus >= 201703L, "RoboFace firmware requires C++17 in every environment");

namespace roboface {

// Kept in step with the repository VERSION file by `release-version`. It is announced nowhere
// on the wire yet -- `hello` carries `proto_ver`, which is the contract version and a different
// thing from the build's -- but a device that cannot say which firmware it is running is a
// device you cannot debug from a serial log.
inline constexpr const char* kFirmwareVersion = "1.3.0";

// The board this build targets. v6 adds the FIRE, at which point this stops being a constant
// and starts being a capability question; until then, saying it plainly beats implying it.
inline constexpr const char* kBoard = "m5stack-cores3";

}  // namespace roboface

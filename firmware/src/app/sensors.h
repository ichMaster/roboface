// The two sensors that had no code, read at a stated rate.
//
// **This file decides nothing.** `pure/motion.h` says what a motion is and `pure/proximity.h` says
// what an approach is; what is left here is asking the hardware and passing the answer on, which is
// the only part a host cannot check and therefore the only part that belongs behind an `M5` include.
//
// The rates are decisions and carry their reasons. Everything else is three lines of glue.

#pragma once

#include <M5Unified.h>

#include "pure/motion.h"

namespace app {

//: How often the accelerometer is read.
//:
//: 50 Hz, and the number comes from the fastest thing being detected. A shake is three reversals
//: inside 600 ms -- so six edges, one every 100 ms -- and sampling must be several times faster
//: than the signal or a reversal falls between two reads and the shake is missed. 20 ms is five
//: times faster, which is enough margin for a hand that shakes harder than average.
//:
//: Faster would cost the loop for nothing: gravity does not move, and the other four motions are
//: slower still.
constexpr uint32_t kImuIntervalMs = 20;

class Imu {
  public:
    //: Start the sensor. Returns false if it is not there -- a board variant without one is a
    //: device with no motion events, not a device that fails to boot (ARCHITECTURE §Hardware
    //: variants; the capability flags land in v6.1).
    bool begin() {
        ready_ = M5.Imu.begin();
        if (ready_) {
            // Take the current orientation as level. A device on a shelf at an angle is not
            // permanently tilted, it is a device on a shelf.
            const roboface::AccelSample first = read(0);
            detector_.level(first);
        }
        return ready_;
    }

    bool isReady() const { return ready_; }

    //: Read and classify, at most once per interval. Returns `kNone` between reads and whenever
    //: nothing happened -- which is almost always, and is the point.
    roboface::Motion tick(uint32_t now_ms) {
        if (!ready_) return roboface::Motion::kNone;
        if (now_ms - last_read_ms_ < kImuIntervalMs) return roboface::Motion::kNone;
        last_read_ms_ = now_ms;
        return detector_.feed(read(now_ms));
    }

  private:
    roboface::AccelSample read(uint32_t now_ms) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        M5.Imu.getAccel(&x, &y, &z);
        return roboface::AccelSample{x, y, z, now_ms};
    }

    roboface::MotionDetector detector_;
    bool ready_ = false;
    uint32_t last_read_ms_ = 0;
};

}  // namespace app

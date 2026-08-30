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
#include "pure/proximity.h"

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

//: How often the proximity sensor is read.
//:
//: 100 Hz would be pointless: a hand reaching for a desk object takes 300-500 ms, and the settle
//: time in `pure/proximity.h` is 150. 50 ms samples that three times over, which is enough to
//: confirm without the loop paying for readings nothing looks at.
constexpr uint32_t kProximityIntervalMs = 50;

class Proximity {
  public:
    //: Start the sensor, and **prove it is there** (code review #1).
    //:
    //: Two things were wrong and they compounded. The reads went to `Ex_I2C` -- Port A, the external
    //: connector -- while the LTR-553 sits on the **internal** bus beside the AXP2101, the touch
    //: controller and the IMU. So every read failed, the count was always zero, and no approach was
    //: ever detected.
    //:
    //: And `begin()` returned whatever the bus's own `begin()` returned, which succeeds whatever is
    //: connected. So the "not present" line never printed and the boot log said everything was
    //: fine: a feature that silently did nothing, on a board that reported itself healthy.
    //:
    //: Asking the sensor for its part ID is the difference between checking that a bus exists and
    //: checking that a sensor answers.
    bool begin() {
        if (!M5.In_I2C.begin()) return false;

        constexpr uint8_t kPartIdRegister = 0x86;
        constexpr uint8_t kExpectedPartId = 0x92;
        uint8_t part_id = 0;
        ready_ = M5.In_I2C.readRegister(kAddress, kPartIdRegister, &part_id, 1, kI2cHz) &&
                 part_id == kExpectedPartId;
        part_id_ = part_id;
        return ready_;
    }

    bool isReady() const { return ready_; }

    //: What the part-ID register actually answered. `0x92` is an LTR-553; `0x00` is nothing on the
    //: bus, which is a different problem from the wrong chip answering and deserves a different
    //: word in the log.
    uint8_t partId() const { return part_id_; }

    roboface::Presence tick(uint32_t now_ms) {
        if (!ready_) return roboface::Presence::kNone;
        if (now_ms - last_read_ms_ < kProximityIntervalMs) return roboface::Presence::kNone;
        last_read_ms_ = now_ms;
        return detector_.feed(read(), now_ms);
    }

    bool isNear() const { return detector_.isNear(); }

  private:
    //: The LTR-553's proximity register, read over the shared I2C bus. A count, not a distance --
    //: see the note in `pure/proximity.h` about why the thresholds are stated in counts.
    uint16_t read() {
        constexpr uint8_t kProximityDataRegister = 0x8D;
        uint8_t bytes[2] = {0, 0};
        if (!M5.In_I2C.readRegister(kAddress, kProximityDataRegister, bytes, 2, kI2cHz)) {
            return 0;
        }
        return static_cast<uint16_t>(bytes[0] | ((bytes[1] & 0x07) << 8));
    }

    //: The LTR-553's address on the internal bus, and the speed to talk to it at.
    static constexpr uint8_t kAddress = 0x23;
    static constexpr uint32_t kI2cHz = 100000;

    roboface::ProximityDetector detector_;
    bool ready_ = false;
    uint8_t part_id_ = 0;
    uint32_t last_read_ms_ = 0;
};

}  // namespace app

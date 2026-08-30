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
        const bool answered = M5.In_I2C.readRegister(kAddress, kPartIdRegister, &part_id, 1, kI2cHz);
        part_id_ = part_id;
        if (!answered || part_id != kExpectedPartId) {
            ready_ = false;
            return false;
        }

        // **And then switch it on** -- which the first version did not, and that is why the sensor
        // answered its part ID and measured nothing.
        //
        // The LTR-553 powers up in **standby**, where the proximity data register reads 0 forever.
        // Zero is below every threshold, so `ProximityDetector` sat in "nothing there" exactly as
        // it did when the reads went to the wrong bus -- the same silent nothing, one layer deeper,
        // and `begin()` reported success again because asking a chip its name does not start it.
        //
        // Registers per the LTR-553 datasheet:
        //   0x82 PS_LED        60 kHz, 100% duty, 100 mA -- the emitter has to reach a hand
        //   0x83 PS_N_PULSES   8 pulses averaged per measurement
        //   0x84 PS_MEAS_RATE  50 ms, matching this class's own read interval
        //   0x81 PS_CONTR      0x03 = active, gain x16
        M5.In_I2C.writeRegister8(kAddress, 0x82, 0x7F, kI2cHz);
        M5.In_I2C.writeRegister8(kAddress, 0x83, 0x08, kI2cHz);
        M5.In_I2C.writeRegister8(kAddress, 0x84, 0x02, kI2cHz);
        M5.In_I2C.writeRegister8(kAddress, 0x81, 0x03, kI2cHz);

        // Read it back. A write that did not land looks exactly like a sensor that is not there,
        // and this whole subsystem has now twice been defeated by a check that asked an easier
        // question than the one that mattered.
        uint8_t control = 0;
        ready_ = M5.In_I2C.readRegister(kAddress, 0x81, &control, 1, kI2cHz) &&
                 (control & 0x03) == 0x03;
        control_ = control;
        return ready_;
    }

    bool isReady() const { return ready_; }

    //: What the part-ID register actually answered. `0x92` is an LTR-553; `0x00` is nothing on the
    //: bus, which is a different problem from the wrong chip answering and deserves a different
    //: word in the log.
    uint8_t partId() const { return part_id_; }

    //: What `PS_CONTR` read back after `begin()` wrote it. `0x03` is active; `0x00` is a sensor
    //: sitting in standby, which is what the first version left it in.
    uint8_t control() const { return control_; }

    //: The raw count, for `/sensors`. Exposed because the thresholds in `pure/proximity.h` are
    //: stated in counts and nothing on a laptop can tell you what a hand 10 cm away measures.
    uint16_t rawCount() { return read(); }

    //: The highest count seen since boot -- the number that says whether a hand registers at all.
    uint16_t peakCount() const { return peak_count_; }
    void resetPeak() { peak_count_ = 0; }

    roboface::Presence tick(uint32_t now_ms) {
        if (!ready_) return roboface::Presence::kNone;
        if (now_ms - last_read_ms_ < kProximityIntervalMs) return roboface::Presence::kNone;
        last_read_ms_ = now_ms;
        const uint16_t count = read();
        // **The peak since boot**, because the person waving a hand and the person reading serial
        // are not the same person and a poll that misses the hand is indistinguishable from a hand
        // that reads nothing. Same reason `/touch` keeps a ring: the measurement has to survive
        // until someone can look at it.
        if (count > peak_count_) peak_count_ = count;
        return detector_.feed(count, now_ms);
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
    uint8_t control_ = 0;
    uint16_t peak_count_ = 0;
    uint32_t last_read_ms_ = 0;
};

}  // namespace app

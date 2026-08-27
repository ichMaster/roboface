// The input-level envelope: what the meter draws.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>. Arithmetic over samples, so a
// laptop can prove that silence reads as silence and a full-scale tone reads as full -- neither of
// which is obvious by looking at a 28 px bar on a desk.
//
// **Peak, not RMS.** RMS is the better measure of loudness and the worse one for a meter: it lags,
// and a meter that lags does not look like a level, it looks like a bug. The person is using this
// to confirm the device is hearing them *now*, so the number that matters is the loudest sample in
// the frame rather than the average energy of it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

//: How much of the previous level survives into the next frame. Frames are 20 ms, so a meter driven
//: straight from the peak flickers at 50 Hz; this is a decay, not a smoothing filter -- it follows
//: a rise immediately and eases a fall, which is what a level meter is expected to do.
inline constexpr float kLevelDecay = 0.6f;

// The loudest sample in a frame, as 0..1. Returns 0 for an empty frame rather than dividing.
inline float peakLevel(const int16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) return 0.0f;
    int32_t peak = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // Negated rather than abs(): INT16_MIN has no positive counterpart, and abs() on it is
        // undefined. Clamping to INT16_MAX loses one count of a value nothing can hear.
        const int32_t value = samples[i] < 0 ? -static_cast<int32_t>(samples[i] + 1) : samples[i];
        if (value > peak) peak = value;
    }
    return static_cast<float>(peak) / 32767.0f;
}

// Follows a rise at once and eases a fall. The asymmetry is the point: a meter that smoothed both
// directions would under-report the start of every word.
inline float decayToward(float previous, float next, float decay = kLevelDecay) {
    if (next >= previous) return next;
    return previous * decay + next * (1.0f - decay);
}

// How many of `total` bars a level lights. Bars rather than a continuous height because the band is
// 28 px: a smooth bar would move in steps anyway, and stating the steps makes them even.
inline std::size_t barsForLevel(float level, std::size_t total) {
    if (level <= 0.0f || total == 0) return 0;
    if (level >= 1.0f) return total;
    const auto lit = static_cast<std::size_t>(level * static_cast<float>(total) + 0.5f);
    return lit > total ? total : lit;
}

}  // namespace roboface

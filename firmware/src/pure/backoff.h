// Exponential backoff with jitter, for the reconnect supervisors.
//
// Two properties, and the second is the one that is easy to leave out:
//
//   * **A ceiling.** Unbounded doubling means a device that lost its router at 3 a.m. is waiting
//     nine hours by breakfast. The ceiling is what makes "it reconnects on its own" true on a
//     timescale a person would call on its own.
//   * **Jitter.** Several devices rebooting after a power cut would otherwise retry in lockstep,
//     hammering the server in synchronised waves and making the outage look like a load problem.
//     It is not decoration; it is the difference between N clients and one thundering herd.
//
// The jitter source is injected rather than read from a global, so tests are deterministic and
// two instances on one board can genuinely diverge (app/net.cpp seeds it per-device).

#pragma once

#include <cstdint>

namespace roboface {

class Backoff {
  public:
    // Defaults chosen for a desk device: quick enough that a brief WiFi blip is invisible, capped
    // low enough that a person who fixes the router does not then wait minutes for the device to
    // notice.
    static constexpr uint32_t kDefaultBaseMs = 500;
    static constexpr uint32_t kDefaultCeilingMs = 30000;
    static constexpr uint32_t kDefaultMultiplier = 2;

    constexpr Backoff(uint32_t base_ms = kDefaultBaseMs, uint32_t ceiling_ms = kDefaultCeilingMs,
                      uint32_t multiplier = kDefaultMultiplier)
        : base_ms_(base_ms == 0 ? 1 : base_ms),
          ceiling_ms_(ceiling_ms < base_ms ? base_ms : ceiling_ms),
          multiplier_(multiplier < 2 ? 2 : multiplier),
          current_ms_(base_ms == 0 ? 1 : base_ms) {}

    // Not `constexpr`: these mutate, they are only ever called at run time -- from a supervisor,
    // with a draw from the hardware RNG -- and a mutating constexpr member is a C++14 feature the
    // board's toolchain does not reliably offer. The const accessors keep theirs.
    //
    // The delay before the next attempt, then grow. `jitter_permille` is 0..1000 and is applied as
    // a *subtraction* of up to 25% -- retrying early is harmless, retrying late compounds with the
    // ceiling, and only spreading downward keeps the ceiling meaningful.
    uint32_t nextDelayMs(uint16_t jitter_permille = 0) {
        const uint32_t base = current_ms_;
        grow();

        const uint32_t spread = base / 4;
        const uint32_t reduction = spread == 0 ? 0 : (spread * (jitter_permille % 1001)) / 1000;
        return base - reduction;
    }

    // The next delay without consuming it -- for a status line, or a test that wants to look.
    constexpr uint32_t peekDelayMs() const { return current_ms_; }

    // Call on a successful connect. Without this a device that flaps once an hour would keep
    // climbing toward the ceiling all day and eventually respond to a two-second outage with a
    // thirty-second wait.
    void reset() { current_ms_ = base_ms_; }

    constexpr uint32_t attempts() const { return attempts_; }

  private:
    void grow() {
        ++attempts_;
        if (current_ms_ >= ceiling_ms_ / multiplier_) {
            current_ms_ = ceiling_ms_;
            return;
        }
        current_ms_ *= multiplier_;
    }

    uint32_t base_ms_;
    uint32_t ceiling_ms_;
    uint32_t multiplier_;
    uint32_t current_ms_;
    uint32_t attempts_ = 0;
};

}  // namespace roboface

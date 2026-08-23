// WiFi: bring it up, keep it up.
//
// Glue (`namespace app`) — it owns the radio and holds no decisions. *When* to retry comes from
// `roboface::Backoff`, which is pure and tested on a host; this module only acts on the answer.
// That split is what makes the reconnect behaviour provable without standing next to a router.
//
// It also does not set device state. `net` reports what the link is doing; the state machine
// decides what that means. Two things deciding the same state is how a device ends up showing
// `idle` while offline.

#pragma once

#include <cstdint>

#include "pure/backoff.h"

namespace app {

class Net {
  public:
    enum class Link {
        kDown,        // no association, and not currently trying
        kConnecting,  // an attempt is in flight
        kUp,          // associated, with an address
    };

    // `now_ms` is passed in rather than read from millis() inside, so the supervisor's timing is
    // driven by the caller and this class keeps no clock of its own.
    void begin(const char* ssid, const char* password, uint32_t now_ms);

    // Step from the main loop. Returns true when the link state changed, so the caller can raise
    // the corresponding device event exactly once rather than polling for a difference.
    bool loop(uint32_t now_ms);

    Link link() const { return link_; }
    bool isUp() const { return link_ == Link::kUp; }

    // For the status line. The address is the one fact a person debugging a connection actually
    // wants, and it is not a secret -- unlike the password, which appears nowhere in this class's
    // output by construction.
    const char* ipAddress() const { return ip_; }
    uint32_t attempts() const { return backoff_.attempts(); }
    uint32_t nextRetryInMs(uint32_t now_ms) const;

  private:
    void startAttempt(uint32_t now_ms);
    uint16_t jitterPermille();

    const char* ssid_ = nullptr;
    const char* password_ = nullptr;

    Link link_ = Link::kDown;
    roboface::Backoff backoff_{};
    uint32_t retry_at_ms_ = 0;
    uint32_t attempt_started_ms_ = 0;
    char ip_[16] = "0.0.0.0";
    bool started_ = false;
};

}  // namespace app

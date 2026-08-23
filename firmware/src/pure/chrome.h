// Chrome: what is on the screen besides the face, and when it stops being there.
//
// features/DEVICE_UI.md §Rules gives two that shape everything here:
//
//   * "Nothing permanent but a problem." Every indicator fades ~3 s after it settles. What stays
//     is exactly an unresolved fault, a live camera and a muted microphone -- the last two arrive
//     in v2 and v3; v0.4 has the fault.
//   * "Never block the face." Chrome lives in the outer 28 px band; the face keeps the central
//     264x184.
//
// The rules are almost entirely about **time**, which is why they are here rather than in the
// drawing code: with an injected clock a test proves the three-second fade in microseconds, where
// on a board it would mean watching a screen for four seconds and believing what you saw.
//
// Chrome is driven by **device-local facts** -- link, charge, fault -- and explicitly *not* by
// `EmotionFrame` (DEVICE_UI §What this adds to the contracts). v0.4 does not touch that seam.

#pragma once

#include <cstdint>

#include "pure/ws_protocol.h"

namespace roboface {

// DEVICE_UI §Motion and timing, verbatim. Named so the drawing code in RF-021 uses the documented
// numbers rather than inventing its own -- and so changing one changes it everywhere.
inline constexpr uint32_t kChromeFadeInMs = 120;
inline constexpr uint32_t kChromeFadeOutMs = 400;
inline constexpr uint32_t kSettleHideMs = 3000;

// DEVICE_UI §Indicators.
inline constexpr int kLowBatteryPercent = 20;

enum class LinkState {
    kConnecting,  // arcs cycle
    kDegraded,    // one arc, amber
    kOffline,     // crossed, amber
    kConnected,   // settles, then hides
};

// What the bottom band is showing. **Single-tenant** by rule, priority carousel > toast > level
// (DEVICE_UI §Layout). v0.4 only ever produces `kFault`, but the arbitration belongs here now:
// v1 adds the level meter and v2.6 the carousel, and a band that grew two tenants without a rule
// would show both on top of each other.
enum class BandTenant {
    kNothing,
    kLevel,     // v1 — the input-level meter
    kFault,     // this version — the enumerated error code
    kCarousel,  // v2.6 — the skin carousel
};

// The device-local facts chrome is drawn from.
struct ChromeFacts {
    LinkState link = LinkState::kConnecting;
    int battery_percent = 100;
    bool charging = false;
    bool fault_active = false;
    ErrorCode fault = ErrorCode::kUnknown;
    // v1 / v2.6 set these; declared now so the band arbitration is complete rather than growing a
    // branch per version.
    bool level_meter_wanted = false;
    bool carousel_wanted = false;
};

struct ChromeVisibility {
    bool link = false;
    bool battery = false;
    BandTenant band = BandTenant::kNothing;
};

class Chrome {
  public:
    // Feed the current facts and the clock. Returns nothing: `visibility()` is the question, and
    // keeping them separate means a caller can ask twice in one frame without advancing time.
    void update(uint32_t now_ms, const ChromeFacts& facts) {
        // Any change restarts the settle timer. Without this a link that flaps every two seconds
        // would keep counting down through the changes and blink itself invisible -- the indicator
        // would be least visible exactly when it mattered most.
        if (facts.link != facts_.link) settled_since_ms_ = now_ms;
        if (facts.battery_percent != facts_.battery_percent || facts.charging != facts_.charging) {
            settled_since_ms_ = now_ms;
        }
        if (!started_) {
            settled_since_ms_ = now_ms;
            started_ = true;
        }
        facts_ = facts;
        now_ms_ = now_ms;
    }

    ChromeVisibility visibility() const {
        ChromeVisibility shown;

        // Link: visible whenever it is not simply working. Once connected it settles and hides,
        // because a working link is not news.
        if (facts_.link == LinkState::kConnected) {
            shown.link = (now_ms_ - settled_since_ms_) < kSettleHideMs;
        } else {
            shown.link = true;
        }

        // Battery: below 20 %, or while charging (the fill animates). Above 20 % on battery it is
        // not worth a person's attention, and DEVICE_UI is explicit that charge is otherwise
        // expressed as the face's own tiredness rather than as a number.
        if (facts_.charging) {
            shown.battery = true;
        } else if (facts_.battery_percent < kLowBatteryPercent) {
            shown.battery = true;
        } else {
            shown.battery = false;
        }

        shown.band = band();
        return shown;
    }

    // The single tenant of the bottom band, by documented priority.
    BandTenant band() const {
        if (facts_.carousel_wanted) return BandTenant::kCarousel;
        // A fault never auto-dismisses -- no timer is consulted here, and that omission is the
        // rule. It is also what makes the fading safe for everything else: the one thing a person
        // must not miss is the one thing that cannot disappear on its own.
        if (facts_.fault_active) return BandTenant::kFault;
        if (facts_.level_meter_wanted) return BandTenant::kLevel;
        return BandTenant::kNothing;
    }

    // The facts, for the drawing code. Exposed deliberately rather than left for the caller to
    // keep its own copy: two places holding the same facts is how a battery pill ends up showing a
    // level the fade logic has already decided is stale.
    ErrorCode fault() const { return facts_.fault; }
    bool faultActive() const { return facts_.fault_active; }
    LinkState link() const { return facts_.link; }
    int batteryPercent() const { return facts_.battery_percent; }
    bool charging() const { return facts_.charging; }

    // How long the current indicator state has been settled -- the drawing code uses this to place
    // itself within the 120 ms in / 400 ms out fade rather than tracking its own clock.
    uint32_t settledForMs() const { return now_ms_ - settled_since_ms_; }

  private:
    ChromeFacts facts_{};
    uint32_t now_ms_ = 0;
    uint32_t settled_since_ms_ = 0;
    bool started_ = false;
};

}  // namespace roboface

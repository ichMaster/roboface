#include "app/net.h"

#include <WiFi.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

namespace app {
namespace {

// How long one association attempt is given before it is abandoned and rescheduled. Long enough
// for a slow router, short enough that a wrong password does not look like a hang.
constexpr uint32_t kAttemptTimeoutMs = 12000;

}  // namespace

void Net::begin(const char* ssid, const char* password, uint32_t now_ms) {
    ssid_ = ssid;
    password_ = password;
    started_ = true;

    WiFi.mode(WIFI_STA);
    // The ESP32 persists credentials in NVS by default and will happily reconnect to a *stale*
    // network behind our back, which makes a config change look like it did not take.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);

    startAttempt(now_ms);
}

void Net::startAttempt(uint32_t now_ms) {
    link_ = Link::kConnecting;
    attempt_started_ms_ = now_ms;

    // Deliberately logs the SSID and never the password. The server's rule -- a log has to stay
    // safe to paste into an issue -- applies to serial output too, and serial is the one channel
    // a person is most likely to copy from.
    Serial.printf("[net] connecting to \"%s\" (attempt %lu)\n", ssid_ ? ssid_ : "",
                  static_cast<unsigned long>(backoff_.attempts() + 1));

    WiFi.disconnect(true);
    WiFi.begin(ssid_, password_);
}

uint16_t Net::jitterPermille() {
    // Hardware RNG, so two boards on one router genuinely diverge after a power cut rather than
    // retrying in lockstep. `roboface::Backoff` only ever shortens a delay with this, so a bad
    // draw cannot push a retry past the ceiling.
    return static_cast<uint16_t>(esp_random() % 1001);
}

bool Net::loop(uint32_t now_ms) {
    if (!started_) return false;

    const Link previous = link_;

    switch (link_) {
        case Link::kConnecting: {
            if (WiFi.status() == WL_CONNECTED) {
                link_ = Link::kUp;
                backoff_.reset();
                std::snprintf(ip_, sizeof(ip_), "%s", WiFi.localIP().toString().c_str());
                Serial.printf("[net] up, ip %s\n", ip_);
                break;
            }
            if (now_ms - attempt_started_ms_ >= kAttemptTimeoutMs) {
                const uint32_t delay_ms = backoff_.nextDelayMs(jitterPermille());
                retry_at_ms_ = now_ms + delay_ms;
                link_ = Link::kDown;
                Serial.printf("[net] attempt failed, retrying in %lu ms\n",
                              static_cast<unsigned long>(delay_ms));
            }
            break;
        }

        case Link::kUp: {
            if (WiFi.status() != WL_CONNECTED) {
                std::snprintf(ip_, sizeof(ip_), "0.0.0.0");
                // Retry immediately on the first loss: a brief flap should be invisible, and the
                // backoff is there for a router that is actually gone, not for a dropped packet.
                retry_at_ms_ = now_ms;
                link_ = Link::kDown;
                Serial.println("[net] link lost");
            }
            break;
        }

        case Link::kDown: {
            if (now_ms >= retry_at_ms_) startAttempt(now_ms);
            break;
        }
    }

    return link_ != previous;
}

uint32_t Net::nextRetryInMs(uint32_t now_ms) const {
    if (link_ != Link::kDown) return 0;
    return retry_at_ms_ > now_ms ? retry_at_ms_ - now_ms : 0;
}

}  // namespace app

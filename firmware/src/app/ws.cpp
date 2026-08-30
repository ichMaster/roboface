#include "app/ws.h"

#include <WebSocketsClient.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

namespace app {
namespace {

WebSocketsClient client;
Ws* active = nullptr;

// How long one connection attempt is given before the supervisor gives up on it and schedules
// another. Without this the supervisor raced its own first attempt: `begin()` opened the socket
// and the next `loop()` -- microseconds later, with retry_at_ms_ still 0 -- tore it down to start
// again, so every boot threw away its first attempt.
constexpr uint32_t kConnectTimeoutMs = 8000;

// The library will not attempt a connection sooner than its reconnect interval, so this is the
// delay before the *first* attempt as well as the floor for later ones. Small: the first attempt
// should be prompt, and everything after it is paced by our own backoff.
constexpr uint32_t kInitialReconnectMs = 200;

constexpr uint32_t kPingIntervalMs = 15000;
constexpr uint32_t kPongTimeoutMs = 5000;
constexpr uint8_t kMissedPongsBeforeDrop = 2;

}  // namespace

void Ws::begin(const char* url, const char* device_id, uint32_t now_ms) {
    device_id_ = device_id;
    active = this;
    started_ = true;

    const roboface::WsUrl parsed = roboface::parseWsUrl(url);
    if (!parsed.valid) {
        // A typo in config.h is a configuration fault, not a connection that silently never
        // happens. Say so once, loudly, rather than retrying a nonsense address forever.
        Serial.printf("[ws] SERVER_URL is not a ws:// URL: \"%s\"\n", url ? url : "(null)");
        started_ = false;
        return;
    }

    host_ = parsed.host;
    port_ = parsed.port;
    path_ = parsed.path;
    if (parsed.secure) {
        // Accepted, and still plaintext: the server has no TLS. Saying so beats a silent
        // downgrade that would look like a working secure connection.
        Serial.println("[ws] SERVER_URL says wss:// but the transport is plaintext (no TLS "
                       "server-side yet)");
    }

    Serial.printf("[ws] server %s:%u%s\n", host_.c_str(), static_cast<unsigned>(port_),
                  path_.c_str());

    client.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        if (active == nullptr) return;
        switch (type) {
            case WStype_CONNECTED:
                active->connected_ = true;
                active->send_failures_ = 0;
                active->backoff_.reset();
                client.setReconnectInterval(kInitialReconnectMs);
                Serial.println("[ws] connected");
                // hello first, always: the server answers any other opening frame with
                // bad_frame, and a wrong proto_ver with proto_unsupported and a close.
                client.sendTXT(
                    roboface::buildHello(active->device_id_, roboface::Caps{}, active->proto_ver_)
                        .c_str());
                if (active->handler_) active->handler_(Event::kConnected, roboface::ServerFrame{});
                break;

            case WStype_DISCONNECTED:
                if (active->connected_) Serial.println("[ws] disconnected");
                active->connected_ = false;
                if (active->handler_) {
                    active->handler_(Event::kDisconnected, roboface::ServerFrame{});
                }
                break;

            case WStype_TEXT:
                active->handleText(reinterpret_cast<const char*>(payload), length);
                break;

            case WStype_BIN:
                // A binary frame carries no envelope; its meaning comes from what the connection
                // is doing (`server_binary_meaning`), which from v1.1 makes it `tts_audio`. The
                // count stays -- a bring-up wants to see frames arriving even when nothing is
                // playing them yet.
                ++active->binary_frames_;
                if (active->binary_handler_) active->binary_handler_(payload, length);
                break;

            default:
                break;
        }
    });

    connect(now_ms);
    // Let the attempt `connect()` just started actually run. Leaving this at 0 meant the first
    // loop() aborted it immediately.
    retry_at_ms_ = now_ms + kConnectTimeoutMs;
}

void Ws::connect(uint32_t now_ms) {
    (void)now_ms;
    client.begin(host_.c_str(), port_, path_.c_str());
    client.enableHeartbeat(kPingIntervalMs, kPongTimeoutMs, kMissedPongsBeforeDrop);
    // **The library's loop() is what opens the connection at all**, not merely what reopens it:
    //
    //     if (!clientIsConnected(&_client)) {
    //         if ((millis() - _lastConnectionFail) < _reconnectInterval) return;   // <-- gate
    //         ... connect ...
    //     }
    //
    // `_lastConnectionFail` starts at 0, so a huge interval makes that gate true from the first
    // millisecond and the client never connects even once. An earlier version set 0xFFFFFFFF here
    // to "stop the library racing us" and thereby stopped it working entirely.
    //
    // The schedule still comes from `roboface::Backoff` -- `loop()` feeds each computed delay in
    // below -- so the tested logic remains the logic that runs. This is just the floor.
    client.setReconnectInterval(kInitialReconnectMs);
}

void Ws::handleText(const char* payload, std::size_t length) {
    // Bounded by what *this device* can afford, which is not what the server can afford. See
    // kMaxInboundFrameBytes: 64 KiB is the contract's ceiling and a fifth of this board's RAM.
    if (length > roboface::kMaxInboundFrameBytes) {
        Serial.printf("[ws] dropped a %u-byte frame (device limit %u)\n",
                      static_cast<unsigned>(length),
                      static_cast<unsigned>(roboface::kMaxInboundFrameBytes));
        if (handler_) {
            roboface::ServerFrame oversize;
            oversize.result = roboface::ParseResult::kOversize;
            handler_(Event::kDropped, oversize);
        }
        return;
    }

    const roboface::ServerFrame frame = roboface::parseServerFrame(payload, length);

    switch (frame.result) {
        case roboface::ParseResult::kReply:
        case roboface::ParseResult::kAsrPartial:
        case roboface::ParseResult::kAsr:
        // **Added here as well as to the parser, and that is not bookkeeping.** A result the
        // parser produces and this list omits falls through to `kUnsupported` and is logged as
        // "not handled" -- one layer below `main.cpp`, where nobody looking at the face would
        // think to check. v1.1 lost half a session to exactly that, with `tts_end`.
        case roboface::ParseResult::kEmotion:
        case roboface::ParseResult::kTtsEnd:
        case roboface::ParseResult::kError:
        case roboface::ParseResult::kPong:
            if (handler_) handler_(Event::kFrame, frame);
            return;

        case roboface::ParseResult::kUnsupported:
            // A declared type this build does not handle -- every v1 and v2 frame looks like this
            // to a v0.3 device. Noted, not treated as a fault: the server is not broken.
            Serial.printf("[ws] ignoring %s (not handled in v0.3)\n", frame.type.c_str());
            if (handler_) handler_(Event::kDropped, frame);
            return;

        case roboface::ParseResult::kOversize:
            Serial.printf("[ws] dropped an oversize frame (%u bytes)\n",
                          static_cast<unsigned>(length));
            if (handler_) handler_(Event::kDropped, frame);
            return;

        case roboface::ParseResult::kMalformed:
            Serial.println("[ws] dropped a malformed frame");
            if (handler_) handler_(Event::kDropped, frame);
            return;
    }
}

void Ws::loop(uint32_t now_ms) {
    if (!started_) return;

    client.loop();

    if (!connected_ && now_ms >= retry_at_ms_) {
        const uint32_t delay_ms = backoff_.nextDelayMs(static_cast<uint16_t>(esp_random() % 1001));
        retry_at_ms_ = now_ms + delay_ms;
        // Hand our schedule to the library rather than calling begin() again. begin() resets the
        // whole client, which would abandon a handshake already in flight -- and the library is
        // the thing that actually opens the socket, so the delay it honours has to be ours.
        client.setReconnectInterval(delay_ms);
        // Say what happened and when the next one is due, rather than announcing a delay that is
        // not being waited. Serial is the only diagnostic channel this device has, and a log that
        // misdescribes the code costs the reader the time to disprove it.
        Serial.printf("[ws] attempt %lu failed; next in %lu ms\n",
                      static_cast<unsigned long>(backoff_.attempts()),
                      static_cast<unsigned long>(delay_ms));
    }
}

bool Ws::noteSend(bool ok) {
    // A socket the far end has abandoned does not report itself closed: `sendTXT`/`sendBIN` simply
    // fail, the TCP buffer stays full, and the library keeps the connection flagged up. The device
    // then looks connected, says nothing, and never reconnects -- every write failing with EAGAIN
    // once a second for as long as it is left running. Restarting the *server* does not clear it,
    // because nothing on this side is watching.
    //
    // So watch here. One failure is a full buffer and worth retrying; a run of them is a dead link.
    if (ok) {
        send_failures_ = 0;
        return true;
    }
    if (++send_failures_ >= kMaxSendFailures) {
        Serial.printf("[ws] %u consecutive send failures -- treating the link as dead\n",
                      static_cast<unsigned>(send_failures_));
        send_failures_ = 0;
        connected_ = false;
        client.disconnect();
    }
    return false;
}

bool Ws::sendTextIn(const char* text) {
    if (!connected_) return false;
    return noteSend(client.sendTXT(roboface::buildTextIn(text).c_str()));
}

bool Ws::sendEvent(roboface::EventType type, const char* kind,
                   const char* meta_key, const char* meta_value,
                   const char* count_key, int count) {
    if (!connected_) return false;
    return noteSend(client.sendTXT(
        roboface::buildEvent(type, kind, meta_key, meta_value, count_key, count).c_str()));
}

bool Ws::sendListenStart() {
    if (!connected_) return false;
    return noteSend(client.sendTXT(roboface::buildListenStart().c_str()));
}

bool Ws::sendAudio(const uint8_t* data, std::size_t length) {
    if (!connected_) return false;
    // No envelope, by contract. A device that framed this as JSON would double its size and the
    // server would refuse it, because `audio` is declared binary.
    return noteSend(client.sendBIN(data, length));
}

bool Ws::sendListenStop() {
    if (!connected_) return false;
    return noteSend(client.sendTXT(roboface::buildListenStop().c_str()));
}

bool Ws::sendPing() {
    if (!connected_) return false;
    return noteSend(client.sendTXT(roboface::buildPing().c_str()));
}

}  // namespace app

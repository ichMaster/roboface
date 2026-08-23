#include "app/ws.h"

#include <WebSocketsClient.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

namespace app {
namespace {

WebSocketsClient client;
Ws* active = nullptr;

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
                active->backoff_.reset();
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
                // Accepted and counted, not interpreted: binary frames carry no envelope and
                // nothing gives them meaning until v1's audio. Counting them means a v1 bring-up
                // can see they are arriving before anything can play them.
                ++active->binary_frames_;
                break;

            default:
                break;
        }
    });

    connect(now_ms);
}

void Ws::connect(uint32_t now_ms) {
    (void)now_ms;
    client.begin(host_.c_str(), port_, path_.c_str());
    client.enableHeartbeat(kPingIntervalMs, kPongTimeoutMs, kMissedPongsBeforeDrop);
    // The library reconnects on its own schedule; ours is the one that is tested, so it is the
    // one that decides. Setting a huge interval here keeps the library from racing us.
    client.setReconnectInterval(0xFFFFFFFF);
}

void Ws::handleText(const char* payload, std::size_t length) {
    // Bounded before parsing, mirroring the server's own rule. It matters more here: this board
    // has 320 KB of RAM, and an oversize frame that reached the parser would be copied first.
    const roboface::ServerFrame frame = roboface::parseServerFrame(payload, length);

    switch (frame.result) {
        case roboface::ParseResult::kReply:
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
        Serial.printf("[ws] reconnecting in %lu ms\n", static_cast<unsigned long>(delay_ms));
        connect(now_ms);
    }
}

bool Ws::sendTextIn(const char* text) {
    if (!connected_) return false;
    return client.sendTXT(roboface::buildTextIn(text).c_str());
}

bool Ws::sendPing() {
    if (!connected_) return false;
    return client.sendTXT(roboface::buildPing().c_str());
}

}  // namespace app

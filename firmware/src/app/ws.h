// The WebSocket client — the device end of the wire.
//
// Glue (`namespace app`): it owns the socket and the library, and it builds and parses nothing
// itself. Every frame goes through `roboface::ws_protocol`, exactly as `tools/chat.py` goes
// through the server's `protocol.py`. A client that hand-rolls its own JSON becomes a second,
// divergent implementation of the contract, and then a change breaks the device while the tests
// carry on looking fine.
//
// **`ws://`, not `wss://`.** The released server runs uvicorn with no TLS, so a wss-only client
// would not connect to the thing that exists. The scheme lives in `config.h`.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "pure/backoff.h"
#include "pure/ws_protocol.h"
#include "pure/ws_url.h"

namespace app {

class Ws {
  public:
    // What the socket did, for the state machine to interpret. `ws` reports; it does not decide.
    enum class Event {
        kConnected,
        kDisconnected,
        kFrame,   // a parsed server frame is in `frame()`
        kDropped,  // something arrived that could not be used; `frame().result` says why
    };

    using Handler = std::function<void(Event, const roboface::ServerFrame&)>;

    void begin(const char* url, const char* device_id, uint32_t now_ms);
    void loop(uint32_t now_ms);

    void onEvent(Handler handler) { handler_ = std::move(handler); }

    // Send a text_in. Returns false when there is no open socket, so the caller can say so rather
    // than pretending the turn started.
    bool sendTextIn(const char* text);
    bool sendPing();

    bool isConnected() const { return connected_; }
    uint32_t attempts() const { return backoff_.attempts(); }
    uint32_t binaryFramesSeen() const { return binary_frames_; }

    // Only for the RF-017 hardware check that a wrong version is rejected rather than retried
    // forever. Never used in a normal build.
    void setProtoVersionOverride(int proto_ver) { proto_ver_ = proto_ver; }

  private:
    void connect(uint32_t now_ms);
    void handleText(const char* payload, std::size_t length);

    Handler handler_;
    const char* device_id_ = nullptr;
    std::string host_;
    std::string path_ = "/ws";
    uint16_t port_ = 8000;
    int proto_ver_ = roboface::kProtoVersion;

    bool connected_ = false;
    bool started_ = false;
    // A separate instance from `net`'s on purpose: a WiFi flap and a server restart are different
    // failures, and sharing one would let a router blip inflate the delay before a reconnect to a
    // server that was never down.
    roboface::Backoff backoff_{};
    uint32_t retry_at_ms_ = 0;
    uint32_t binary_frames_ = 0;
};

}  // namespace app

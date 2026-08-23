// Splitting `ws://host:port/path` into its parts.
//
// Pure, because it is parsing — and CLAUDE.md's firmware rule is that parsing, decisions and math
// never live behind an `M5` include. It began life inside `app/ws.cpp`, which made a URL typo in
// `config.h` something you could only discover by flashing a board.
//
// **`wss://` is accepted and still connects in plaintext.** The released server runs uvicorn with
// no TLS, so refusing the scheme outright would be pedantry against a URL that is aspirationally
// correct; the transport note in ARCHITECTURE §Contracts records the gap rather than the parser
// pretending it is closed.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace roboface {

struct WsUrl {
    bool valid = false;
    bool secure = false;  // the scheme said wss:// -- recorded, not yet honoured
    std::string host;
    uint16_t port = 0;
    std::string path = "/";
};

inline WsUrl parseWsUrl(const char* url) {
    WsUrl parsed;
    if (url == nullptr) return parsed;

    const char* rest = nullptr;
    if (std::strncmp(url, "ws://", 5) == 0) {
        rest = url + 5;
    } else if (std::strncmp(url, "wss://", 6) == 0) {
        rest = url + 6;
        parsed.secure = true;
    } else {
        return parsed;  // http://, a bare host, or a typo -- all equally unusable
    }

    const char* slash = std::strchr(rest, '/');
    const char* colon = std::strchr(rest, ':');

    if (colon != nullptr && (slash == nullptr || colon < slash)) {
        parsed.host.assign(rest, static_cast<std::size_t>(colon - rest));
        const long port = std::strtol(colon + 1, nullptr, 10);
        if (port <= 0 || port > 65535) return parsed;  // still invalid: leave `valid` false
        parsed.port = static_cast<uint16_t>(port);
    } else {
        parsed.host.assign(rest, slash != nullptr ? static_cast<std::size_t>(slash - rest)
                                                  : std::strlen(rest));
        // The scheme's default. Not 8000: a URL without a port means the well-known one, and
        // guessing the product's port here would hide a missing `:8000` in config.h.
        parsed.port = parsed.secure ? 443 : 80;
    }

    if (slash != nullptr && *slash != '\0') {
        parsed.path.assign(slash);
    } else {
        parsed.path = "/";
    }

    parsed.valid = !parsed.host.empty();
    return parsed;
}

}  // namespace roboface

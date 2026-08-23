// RoboFace firmware configuration.
//
// Copy to `config.h` and fill in. `config.h` is gitignored and MUST NOT be committed: it holds
// the WiFi password, and ARCHITECTURE.md §Configuration and secrets is explicit that the
// firmware holds no model key of its own — only these two credentials and a URL.
//
//     cp src/config.example.h src/config.h && $EDITOR src/config.h

#pragma once

// --- WiFi ------------------------------------------------------------------------------
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"

// --- The server ------------------------------------------------------------------------
// `ws://`, not `wss://`. The released server runs uvicorn with no TLS (server/roboface_server/
// app.py passes no ssl_keyfile/ssl_certfile), so a wss:// URL would not connect to the thing
// that exists. The scheme lives here rather than in the client so that turning TLS on later is
// this line plus a CA bundle, not a rewrite.
//
// The host is the machine running the server, reachable from the board's WiFi network — not
// `localhost`, which on the board means the board.
#define SERVER_URL "ws://192.168.1.64:8000/ws"

// --- Screen ---------------------------------------------------------------------------
// 0..255. The face is the product's whole presence, and a desk companion at full brightness in a
// dark study is a lamp. 120 is legible in daylight without being one.
#define SCREEN_BRIGHTNESS 120

// --- Identity --------------------------------------------------------------------------
// Announced in `hello.device_id` and used to key every server log line for this device.
#define DEVICE_ID "core-s3-01"

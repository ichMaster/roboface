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
//
// It must be a literal IP address. This firmware links no mDNS resolver, so a `.local` name — the
// form the host-side tools default to — does not resolve here and the board would simply never
// connect. The server currently runs on the Linux box `ich-picobox` (see DEPLOYMENT.md); if its
// DHCP lease changes, this line changes and the board is reflashed.
#define SERVER_URL "ws://192.168.1.197:8000/ws"

// --- Screen ---------------------------------------------------------------------------
// 0..255. The face is the product's whole presence, and a desk companion at full brightness in a
// dark study is a lamp. 120 is legible in daylight without being one.
#define SCREEN_BRIGHTNESS 120

// --- Audio ----------------------------------------------------------------------------
// 0..255. The Core S3's speaker is small and close to the listener; full volume is unpleasant
// at desk distance and adds distortion that sounds like a bad voice rather than a loud one.
#define SPEAKER_VOLUME 120

// Microphone input gain (M5Unified's `magnification`). The library default of 16 leaves the Core
// S3's ES7210 reading about 1% of full scale for normal speech at desk distance, which is far too
// quiet for recognition. Raise until speech reads roughly 30-60% on `/loopback`'s peak figure --
// high enough to hear, low enough not to clip.
#define MIC_GAIN 64

// --- Identity --------------------------------------------------------------------------
// Announced in `hello.device_id` and used to key every server log line for this device.
#define DEVICE_ID "core-s3-01"

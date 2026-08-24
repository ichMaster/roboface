// The device side of the WS protocol seam — the mirror of server/roboface_server/protocol.py.
//
// ARCHITECTURE.md §Providers and seams names this file as the firmware half of a contract the
// server's `protocol.py` owns the other half of. Every constant here is read from the released
// server, not from the specification, and the contract test next door pins them so a value that
// drifts fails on a laptop rather than on a desk.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>, no <WiFi.h>, no <WebSockets.h>.
// ArduinoJson is used deliberately — it compiles on the `native` platform, so parsing is testable
// without a board, which is the entire reason the parsing lives here and not in `app/ws.cpp`.
//
// Two distinctions this file exists to preserve, both learned from the server:
//
//   * A **declared but unhandled** message type is not a malformed one. The server draws the same
//     line (`UnsupportedMessage` vs `MalformedFrame`) so it can answer one politely and treat the
//     other as a broken peer; the device needs it so a v1 frame arriving early is a shrug rather
//     than a fault.
//   * An **unrecognised error code** still parses. A device that cannot parse an error is a device
//     that cannot report one, and the whole point of an enumerated code is that it survives the
//     trip even when this build is older than the server.

#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace roboface {

// ---------------------------------------------------------------------------------------
// Constants — mirrored from the released protocol.py
// ---------------------------------------------------------------------------------------

// The wire version. There is no compatibility window: the server rejects anything else with
// `proto_unsupported` and closes the socket, so announcing a wrong one is a hard stop and not a
// degraded mode. One server serves one household's devices and they are flashed together.
inline constexpr int kProtoVersion = 1;

// `format/rate/channels`. Built from three constants server-side so the spelling cannot drift
// from its parts; mirrored here as the literal the device must actually send.
inline constexpr const char* kAudioFmt = "pcm16/16000/1";
inline constexpr int kAudioSampleRate = 16000;
inline constexpr int kAudioChannels = 1;

// The server refuses a larger text frame before parsing it. Mirrored so the device does not send
// what would be refused — and applied to the inbound direction too, which matters more here than
// it does on the server: this board has 320 KB of RAM, not gigabytes.
inline constexpr std::size_t kMaxTextFrameBytes = 65536;

// What **this device** will accept inbound, which is a different question from what the contract
// permits. The server has gigabytes; this board has 320 KB, and ArduinoJson allocates a document to
// parse whatever it is handed -- so a single 64 KiB frame would be a fifth of memory before the
// tree is built, with v1's audio buffers and v2's framebuffer already spoken for.
//
// The largest text frame this device can legitimately receive is a `reply` delta of a few dozen
// bytes; bulk data is binary and never reaches this path. 8 KiB is generous by two orders of
// magnitude and still bounded.
//
// Deliberately a *separate* constant: `kMaxTextFrameBytes` is a pinned contract fact that the
// cross-language test compares against the server, and collapsing the two would make the contract
// test and the memory budget argue with each other.
inline constexpr std::size_t kMaxInboundFrameBytes = 8192;

static_assert(kMaxInboundFrameBytes <= kMaxTextFrameBytes,
              "the device cannot accept more than the contract permits");

// ---------------------------------------------------------------------------------------
// The message vocabulary, both directions
// ---------------------------------------------------------------------------------------

// Declared whole even though v0.3 exchanges only hello/text_in/ping and reply/error/pong — the
// contract is the deliverable, not the subset in use, and the server's own module is written the
// same way. `kAudio`, `kImage` and `kTtsAudio` are **binary** frames: they carry raw payloads with
// no JSON envelope, and their meaning comes from direction plus connection state.
enum class DeviceMessage {
    kHello,
    kListenStart,
    kAudio,
    kListenStop,
    kTextIn,
    kEvent,
    kImageIn,
    kImage,
    kPing,
};

enum class ServerMessage {
    kAsrPartial,
    kAsr,
    kReply,
    kEmotion,
    kTtsAudio,
    kTtsEnd,
    kConfigUpdated,
    kError,
    kRestart,
    kPong,
};

inline const char* toString(DeviceMessage message) {
    switch (message) {
        case DeviceMessage::kHello: return "hello";
        case DeviceMessage::kListenStart: return "listen_start";
        case DeviceMessage::kAudio: return "audio";
        case DeviceMessage::kListenStop: return "listen_stop";
        case DeviceMessage::kTextIn: return "text_in";
        case DeviceMessage::kEvent: return "event";
        case DeviceMessage::kImageIn: return "image_in";
        case DeviceMessage::kImage: return "image";
        case DeviceMessage::kPing: return "ping";
    }
    return "";
}

inline const char* toString(ServerMessage message) {
    switch (message) {
        case ServerMessage::kAsrPartial: return "asr_partial";
        case ServerMessage::kAsr: return "asr";
        case ServerMessage::kReply: return "reply";
        case ServerMessage::kEmotion: return "emotion";
        case ServerMessage::kTtsAudio: return "tts_audio";
        case ServerMessage::kTtsEnd: return "tts_end";
        case ServerMessage::kConfigUpdated: return "config_updated";
        case ServerMessage::kError: return "error";
        case ServerMessage::kRestart: return "restart";
        case ServerMessage::kPong: return "pong";
    }
    return "";
}

// ---------------------------------------------------------------------------------------
// Error codes — all twelve
// ---------------------------------------------------------------------------------------

// `bad_frame` and `internal` split by whose fault it is: the device sent something the server
// could not parse, versus the server itself broke. From v0.4 the screen renders the code verbatim
// (features/DEVICE_UI.md §Screens), so the distinction decides which side a person is told is at
// fault -- which is why the device must carry both rather than collapsing them.
enum class ErrorCode {
    kWifiLost,
    kServerUnreachable,
    kProtoUnsupported,
    kUnauthorized,
    kRateLimited,
    kAsrFailed,
    kLlmTimeout,
    kLlmFailed,
    kTtsFailed,
    kVisionFailed,
    kBadFrame,
    kInternal,
    // Not part of the contract: what this build calls a code the server knows and it does not.
    // Kept last so a future value added server-side degrades to "a fault with an unfamiliar name"
    // rather than to a parse failure.
    kUnknown,
};

inline const char* toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::kWifiLost: return "wifi_lost";
        case ErrorCode::kServerUnreachable: return "server_unreachable";
        case ErrorCode::kProtoUnsupported: return "proto_unsupported";
        case ErrorCode::kUnauthorized: return "unauthorized";
        case ErrorCode::kRateLimited: return "rate_limited";
        case ErrorCode::kAsrFailed: return "asr_failed";
        case ErrorCode::kLlmTimeout: return "llm_timeout";
        case ErrorCode::kLlmFailed: return "llm_failed";
        case ErrorCode::kTtsFailed: return "tts_failed";
        case ErrorCode::kVisionFailed: return "vision_failed";
        case ErrorCode::kBadFrame: return "bad_frame";
        case ErrorCode::kInternal: return "internal";
        case ErrorCode::kUnknown: return "unknown";
    }
    return "unknown";
}

inline ErrorCode errorCodeFrom(const char* name) {
    if (name == nullptr) return ErrorCode::kUnknown;
    for (int index = 0; index <= static_cast<int>(ErrorCode::kInternal); ++index) {
        const auto code = static_cast<ErrorCode>(index);
        if (std::strcmp(name, toString(code)) == 0) return code;
    }
    return ErrorCode::kUnknown;
}

// ---------------------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------------------

// What this board physically has (ARCHITECTURE §Hardware variants). The Core S3 has touch, a
// camera and dual mics; it does **not** have `halo` (that is the optional Bottom3, v5) or
// `buttons` (that is the FIRE, v6). The server tailors what it sends to this, so overstating it
// would mean asking for frames this board cannot act on.
struct Caps {
    bool touch = true;
    bool camera = true;
    bool dual_mic = true;
    bool halo = false;
    bool buttons = false;
};

// ---------------------------------------------------------------------------------------
// Building — device -> server
// ---------------------------------------------------------------------------------------

inline std::string buildHello(const char* device_id, const Caps& caps = Caps{},
                              int proto_ver = kProtoVersion) {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kHello);
    doc["device_id"] = device_id;
    doc["proto_ver"] = proto_ver;
    doc["audio_fmt"] = kAudioFmt;

    // Sorted, so two identical devices produce identical bytes and a wire log stays diffable --
    // the same reason the server sorts its own.
    JsonArray list = doc["caps"].to<JsonArray>();
    if (caps.buttons) list.add("buttons");
    if (caps.camera) list.add("camera");
    if (caps.dual_mic) list.add("dual_mic");
    if (caps.halo) list.add("halo");
    if (caps.touch) list.add("touch");

    std::string out;
    serializeJson(doc, out);
    return out;
}

inline std::string buildTextIn(const char* text) {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kTextIn);
    doc["text"] = text;
    std::string out;
    serializeJson(doc, out);
    return out;
}

inline std::string buildPing() {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kPing);
    std::string out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------------------------------
// Parsing — server -> device
// ---------------------------------------------------------------------------------------

enum class ParseResult {
    kReply,        // reply{text, final}
    kTtsEnd,       // tts_end -- the speaking window is closed (v1.1)
    kError,        // error{code, msg}
    kPong,         // pong
    kUnsupported,  // a declared type this phase does not handle -- not a fault
    kMalformed,    // not JSON, not an object, no type, an unknown type, or a bad field
    kOversize,     // longer than the server would have accepted; refused before parsing
};

struct ServerFrame {
    ParseResult result = ParseResult::kMalformed;

    // reply
    std::string text;
    bool final = false;

    // error
    ErrorCode code = ErrorCode::kUnknown;
    std::string msg;

    // For kUnsupported: what it was, so a log can say so.
    std::string type;
};

// Parse one inbound text frame.
//
// `length` is taken explicitly rather than trusting a terminator: this is fed from a socket, and
// the size check has to happen before any parse allocates a copy of it.
inline ServerFrame parseServerFrame(const char* raw, std::size_t length) {
    ServerFrame frame;

    if (raw == nullptr) return frame;
    if (length > kMaxTextFrameBytes) {
        frame.result = ParseResult::kOversize;
        return frame;
    }

    JsonDocument doc;
    if (deserializeJson(doc, raw, length) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        frame.result = ParseResult::kMalformed;
        return frame;
    }

    JsonVariantConst type = doc["type"];
    if (!type.is<const char*>()) {
        frame.result = ParseResult::kMalformed;
        return frame;
    }
    const char* name = type.as<const char*>();
    frame.type = name;

    if (std::strcmp(name, toString(ServerMessage::kReply)) == 0) {
        JsonVariantConst text = doc["text"];
        JsonVariantConst final_flag = doc["final"];
        if (!text.is<const char*>() || !final_flag.is<bool>()) {
            frame.result = ParseResult::kMalformed;
            return frame;
        }
        frame.result = ParseResult::kReply;
        frame.text = text.as<const char*>();
        frame.final = final_flag.as<bool>();
        return frame;
    }

    if (std::strcmp(name, toString(ServerMessage::kError)) == 0) {
        JsonVariantConst code = doc["code"];
        if (!code.is<const char*>()) {
            frame.result = ParseResult::kMalformed;
            return frame;
        }
        frame.result = ParseResult::kError;
        // An unrecognised code still parses -- see the header note. A device that cannot parse an
        // error is a device that cannot report one.
        frame.code = errorCodeFrom(code.as<const char*>());
        JsonVariantConst msg = doc["msg"];
        if (msg.is<const char*>()) frame.msg = msg.as<const char*>();
        return frame;
    }

    if (std::strcmp(name, toString(ServerMessage::kPong)) == 0) {
        frame.result = ParseResult::kPong;
        return frame;
    }

    // `tts_end` carries no fields. Its whole job is to close the speaking window that the binary
    // frames before it belonged to -- they were `tts_audio` because the server was speaking, not
    // because anything labelled them.
    if (std::strcmp(name, toString(ServerMessage::kTtsEnd)) == 0) {
        frame.result = ParseResult::kTtsEnd;
        return frame;
    }

    // Declared server->device types this phase does not handle: asr_partial, asr, emotion,
    // config_updated, restart. (tts_audio is binary and never arrives here; tts_end is handled
    // above from v1.1.) Answering these with "malformed" would make every later frame look like a
    // broken server.
    for (const auto declared : {ServerMessage::kAsrPartial, ServerMessage::kAsr,
                                ServerMessage::kEmotion, ServerMessage::kConfigUpdated,
                                ServerMessage::kRestart}) {
        if (std::strcmp(name, toString(declared)) == 0) {
            frame.result = ParseResult::kUnsupported;
            return frame;
        }
    }

    frame.result = ParseResult::kMalformed;
    return frame;
}

inline ServerFrame parseServerFrame(const std::string& raw) {
    return parseServerFrame(raw.c_str(), raw.size());
}

}  // namespace roboface

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

#include "pure/face.h"

namespace roboface {

//: What kind of thing happened to the device (ARCHITECTURE §event{}). Mirrors the server's
//: `EventType`; `tests/contract/test_firmware_mirror.py` checks the two agree, because a value one
//: side has never heard of is a reaction the character silently never gives.
enum class EventType : uint8_t {
    kTouch,
    kMotion,
    kProximity,
    kVoice,  // v2.5: where a voice is, from the two microphones
    kCount,
};

inline constexpr const char* toString(EventType type) {
    switch (type) {
        case EventType::kTouch: return "touch";
        case EventType::kMotion: return "motion";
        case EventType::kProximity: return "proximity";
        case EventType::kVoice: return "voice";
        case EventType::kCount: break;
    }
    return "touch";
}


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
                              int proto_ver = kProtoVersion, const char* face_set = nullptr) {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kHello);
    doc["device_id"] = device_id;
    doc["proto_ver"] = proto_ver;
    doc["audio_fmt"] = kAudioFmt;
    // **Which face the device is wearing** (v2.6), so a reconnect does not lose it and the server
    // knows what it is looking at. Omitted rather than sent empty: a field always present and
    // usually meaningless teaches every reader to skip it.
    if (face_set != nullptr) doc["face_set"] = face_set;

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

// `event{}` -- the second level of the reaction model (v2.4).
//
// **The event's own type is nested under `event`**, not spread into the envelope: the envelope's
// `type` names the *message*, which every frame here follows, and spreading them would produce a
// frame the server decodes as an unknown message called `touch`. ARCHITECTURE §event{} shows the
// inner object and now says so explicitly.
//
// `meta` is optional and free-form -- a touch reports a zone and a count, a motion an axis --
// because pinning it would mean a protocol change per sensor. The server caps its size.
inline std::string buildEvent(EventType type, const char* kind,
                              const char* meta_key = nullptr, const char* meta_value = nullptr,
                              const char* count_key = nullptr, int count = 0) {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kEvent);
    JsonObject event = doc["event"].to<JsonObject>();
    event["type"] = toString(type);
    event["kind"] = kind;
    if (meta_key != nullptr || count_key != nullptr) {
        JsonObject meta = event["meta"].to<JsonObject>();
        if (meta_key != nullptr && meta_value != nullptr) meta[meta_key] = meta_value;
        if (count_key != nullptr) meta[count_key] = count;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

// Where a voice is (v2.5). Its own builder because `meta.x` must be a **JSON number**: the server
// coerces `meta` from arbitrary network input and refuses anything that is not numeric, so a value
// rendered as `"-0.62"` would arrive, validate, and silently mean "no opinion" -- the failure mode
// this project keeps meeting, where something reports success and does nothing.
inline std::string buildVoiceDirection(float x) {
    JsonDocument doc;
    doc["type"] = toString(DeviceMessage::kEvent);
    JsonObject event = doc["event"].to<JsonObject>();
    event["type"] = toString(EventType::kVoice);
    event["kind"] = "direction";
    JsonObject meta = event["meta"].to<JsonObject>();
    meta["x"] = x;
    std::string out;
    serializeJson(doc, out);
    return out;
}

// The listening window's two frames. Neither carries a field: the binary frames between them are
// `audio` **because the connection is listening** (`device_binary_meaning` on the server), not
// because anything labels them, so these exist only to open and close that window.
inline std::string buildListenStart() {
    return "{\"type\":\"listen_start\"}";
}

inline std::string buildListenStop() {
    return "{\"type\":\"listen_stop\"}";
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
    kEmotion,      // emotion{...} -- the whole face channel (v2.2)
    kAsrPartial,   // asr_partial{text} -- a guess, revised constantly (v1.3)
    kAsr,          // asr{text} -- the resolved utterance (v1.3)
    kTtsEnd,       // tts_end -- the speaking window is closed (v1.1)
    kError,        // error{code, msg}
    kPong,         // pong
    kConfigUpdated,  // config_updated{face_set} -- change the face (v2.6)
    kUnsupported,  // a declared type this phase does not handle -- not a fault
    kMalformed,    // not JSON, not an object, no type, an unknown type, or a bad field
    kOversize,     // longer than the server would have accepted; refused before parsing
};

struct ServerFrame {
    ParseResult result = ParseResult::kMalformed;

    //: `config_updated{face_set}`. **Owned, not a pointer into the document.**
    //:
    //: The first version was `const char*` into the `JsonDocument`, with a comment claiming it was
    //: "valid for as long as the frame is". It was not: the document is a local of
    //: `parseServerFrame` and dies at the return, so every reader saw an empty string. A host test
    //: caught it immediately, which is the argument for the pure/glue split in one line -- on the
    //: board this would have been a face that silently never changed.
    std::string face_set;

    // reply
    std::string text;
    bool final = false;

    // error
    ErrorCode code = ErrorCode::kUnknown;
    std::string msg;

    // emotion (v2.2) -- the whole face channel, as one object rather than six loose fields.
    EmotionFrame emotion;

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
    // `asr_partial` and `asr` differ only in whether the text is still a guess, so they share a
    // shape and are told apart by their result rather than by a field.
    for (const auto declared : {ServerMessage::kAsrPartial, ServerMessage::kAsr}) {
        if (std::strcmp(name, toString(declared)) != 0) continue;
        JsonVariantConst text = doc["text"];
        if (!text.is<const char*>()) {
            frame.result = ParseResult::kMalformed;
            return frame;
        }
        frame.result = declared == ServerMessage::kAsrPartial ? ParseResult::kAsrPartial
                                                              : ParseResult::kAsr;
        frame.text = text.as<const char*>();
        return frame;
    }

    if (std::strcmp(name, toString(ServerMessage::kTtsEnd)) == 0) {
        frame.result = ParseResult::kTtsEnd;
        return frame;
    }

    // **The one frame that is never malformed.** Every other parser above refuses a bad field;
    // this one coerces it, exactly as the server's own decoder does.
    //
    // Two reasons, and neither is leniency for its own sake. A face is not worth dropping a
    // connection over -- the reply is still arriving on the same socket. And the two tiers are
    // separately releasable, so the device may not assume the server sanitised what it sent, any
    // more than the server assumes the device will. Both coerce; the rules are identical and are
    // stated once in ARCHITECTURE §EmotionFrame.
    if (std::strcmp(name, toString(ServerMessage::kConfigUpdated)) == 0) {
        // **Refused here if it is not a string**, and refused *again* by `skinIndexFor` if it is a
        // name nothing answers to. Two checks because they are two different failures: a malformed
        // frame is a protocol fault, and an unknown face is a version disagreement between the two
        // sides -- and only the second one is worth a legible line in a boot log.
        JsonVariantConst face = doc["face_set"];
        if (!face.is<const char*>()) {
            frame.result = ParseResult::kMalformed;
            return frame;
        }
        frame.result = ParseResult::kConfigUpdated;
        frame.face_set = face.as<const char*>();  // copied: the document dies at this return
        return frame;
    }

    if (std::strcmp(name, toString(ServerMessage::kEmotion)) == 0) {
        frame.result = ParseResult::kEmotion;

        JsonVariantConst emotion = doc["emotion"];
        frame.emotion.emotion = emotion.is<const char*>()
                                    ? emotionFrom(emotion.as<const char*>())
                                    : Emotion::kNeutral;

        JsonVariantConst intensity = doc["intensity"];
        frame.emotion.intensity = intensity.is<float>()
                                      ? clampUnit(intensity.as<float>(), 0.0f, 1.0f)
                                      : kDefaultIntensity;

        JsonVariantConst gaze = doc["gaze"];
        if (gaze.is<JsonObjectConst>()) {
            frame.emotion.has_gaze = true;
            JsonVariantConst x = gaze["x"];
            JsonVariantConst y = gaze["y"];
            frame.emotion.gaze_x = x.is<float>() ? clampUnit(x.as<float>(), -1.0f, 1.0f) : 0.0f;
            frame.emotion.gaze_y = y.is<float>() ? clampUnit(y.as<float>(), -1.0f, 1.0f) : 0.0f;
        }

        // `speaking` must be **literally** true. A truthy value is not the flag: it gates the
        // mouth, and `1` arriving as `true` would be a decision made by a coincidence of parsing.
        JsonVariantConst speaking = doc["speaking"];
        frame.emotion.speaking = speaking.is<bool>() && speaking.as<bool>();

        JsonVariantConst ttl = doc["ttl_ms"];
        frame.emotion.ttl_ms = (ttl.is<uint32_t>() && ttl.as<uint32_t>() > 0)
                                   ? ttl.as<uint32_t>()
                                   : kDefaultTtlMs;
        return frame;
    }

    // Declared server->device types this phase does not handle: restart. (tts_audio is binary and
    // never arrives here; tts_end is handled above from v1.1, emotion from v2.2 and config_updated
    // from v2.6.) Answering these with "malformed" would make every later frame look like a broken
    // server -- and this list shrinking by one per phase is the most compact record of the roadmap
    // the firmware has.
    for (const auto declared : {ServerMessage::kRestart}) {
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

// Contract test for the device side of the WS protocol seam.
//
// The authority is server/roboface_server/protocol.py, and this file is deliberately literal: every
// expectation is spelled out rather than derived from the header under test, because a test that
// reads its answers from the implementation cannot notice the implementation moving. Its Python
// counterpart (tests/contract/test_ws_protocol.py) pins the same values from the other side, and
// tests/contract/test_firmware_mirror.py pins the exact bytes this builds.
//
// This file changes only when the contract changes.

#include <unity.h>

#include <set>
#include <string>

#include "pure/ws_protocol.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------

static void test_proto_version_is_pinned() {
    TEST_ASSERT_EQUAL_INT(1, kProtoVersion);
}

static void test_audio_is_pcm16_16khz_mono() {
    TEST_ASSERT_EQUAL_STRING("pcm16/16000/1", kAudioFmt);
    TEST_ASSERT_EQUAL_INT(16000, kAudioSampleRate);
    TEST_ASSERT_EQUAL_INT(1, kAudioChannels);
}

static void test_the_text_frame_cap_matches_the_server() {
    TEST_ASSERT_EQUAL_UINT32(65536u, static_cast<uint32_t>(kMaxTextFrameBytes));
}

// ---------------------------------------------------------------------------------------
// The vocabulary, both directions
// ---------------------------------------------------------------------------------------

static void test_device_to_server_vocabulary_is_exactly_the_contract() {
    const std::set<std::string> expected{"hello",    "listen_start", "audio", "listen_stop",
                                         "text_in",  "event",        "image_in", "image",
                                         "ping"};
    std::set<std::string> actual;
    for (const auto message :
         {DeviceMessage::kHello, DeviceMessage::kListenStart, DeviceMessage::kAudio,
          DeviceMessage::kListenStop, DeviceMessage::kTextIn, DeviceMessage::kEvent,
          DeviceMessage::kImageIn, DeviceMessage::kImage, DeviceMessage::kPing}) {
        actual.insert(toString(message));
    }
    TEST_ASSERT_TRUE(actual == expected);
}

static void test_server_to_device_vocabulary_is_exactly_the_contract() {
    const std::set<std::string> expected{"asr_partial", "asr",   "reply",   "emotion", "tts_audio",
                                         "tts_end",     "config_updated", "error", "restart", "pong"};
    std::set<std::string> actual;
    for (const auto message :
         {ServerMessage::kAsrPartial, ServerMessage::kAsr, ServerMessage::kReply,
          ServerMessage::kEmotion, ServerMessage::kTtsAudio, ServerMessage::kTtsEnd,
          ServerMessage::kConfigUpdated, ServerMessage::kError, ServerMessage::kRestart,
          ServerMessage::kPong}) {
        actual.insert(toString(message));
    }
    TEST_ASSERT_TRUE(actual == expected);
}

// ---------------------------------------------------------------------------------------
// Error codes — all twelve
// ---------------------------------------------------------------------------------------

static void test_error_codes_are_exactly_the_twelve() {
    const std::set<std::string> expected{
        "wifi_lost",  "server_unreachable", "proto_unsupported", "unauthorized",
        "rate_limited", "asr_failed",       "llm_timeout",       "llm_failed",
        "tts_failed", "vision_failed",      "bad_frame",         "internal"};

    std::set<std::string> actual;
    for (int index = 0; index <= static_cast<int>(ErrorCode::kInternal); ++index) {
        actual.insert(toString(static_cast<ErrorCode>(index)));
    }
    TEST_ASSERT_EQUAL_UINT(12u, static_cast<unsigned>(actual.size()));
    TEST_ASSERT_TRUE(actual == expected);
}

static void test_bad_frame_and_internal_are_distinct() {
    // Whose fault it is. From v0.4 the screen renders the code verbatim, so collapsing these
    // would show a person the wrong side as being at fault.
    TEST_ASSERT_EQUAL_STRING("bad_frame", toString(ErrorCode::kBadFrame));
    TEST_ASSERT_EQUAL_STRING("internal", toString(ErrorCode::kInternal));
}

static void test_every_code_round_trips_through_its_name() {
    for (int index = 0; index <= static_cast<int>(ErrorCode::kInternal); ++index) {
        const auto code = static_cast<ErrorCode>(index);
        TEST_ASSERT_EQUAL_INT(index, static_cast<int>(errorCodeFrom(toString(code))));
    }
}

static void test_an_unrecognised_code_degrades_rather_than_failing() {
    // A device that cannot parse an error is a device that cannot report one. A code added
    // server-side after this build was flashed must still arrive as a fault.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kUnknown),
                          static_cast<int>(errorCodeFrom("a_code_from_the_future")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kUnknown),
                          static_cast<int>(errorCodeFrom(nullptr)));
}

// ---------------------------------------------------------------------------------------
// hello
// ---------------------------------------------------------------------------------------

static void test_hello_is_exactly_what_the_server_accepts() {
    // Byte for byte. tests/contract/test_firmware_mirror.py feeds this same literal to the
    // server's own decode(), so a drift on either side fails on one of the two.
    const std::string hello = buildHello("core-s3-01");

    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"hello\",\"device_id\":\"core-s3-01\",\"proto_ver\":1,"
        "\"audio_fmt\":\"pcm16/16000/1\",\"caps\":[\"camera\",\"dual_mic\",\"touch\"]}",
        hello.c_str());
}

static void test_the_core_s3_announces_its_real_hardware() {
    // Not halo (the optional Bottom3, v5) and not buttons (the FIRE, v6). The server tailors what
    // it sends to this, so overstating it means asking for frames this board cannot act on.
    const Caps caps;
    TEST_ASSERT_TRUE(caps.touch);
    TEST_ASSERT_TRUE(caps.camera);
    TEST_ASSERT_TRUE(caps.dual_mic);
    TEST_ASSERT_FALSE(caps.halo);
    TEST_ASSERT_FALSE(caps.buttons);
}

static void test_a_wrong_proto_version_can_be_announced_deliberately() {
    // RF-017's DoD check needs this: announce 99 and the server must reject with
    // proto_unsupported rather than the device silently retrying forever.
    const std::string hello = buildHello("core-s3-01", Caps{}, 99);

    TEST_ASSERT_TRUE(hello.find("\"proto_ver\":99") != std::string::npos);
}

static void test_text_in_and_ping_are_what_the_server_expects() {
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"text_in\",\"text\":\"привіт\"}",
                             buildTextIn("привіт").c_str());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"ping\"}", buildPing().c_str());
}

// ---------------------------------------------------------------------------------------
// Parsing a streamed reply — the shape v0.2 introduced
// ---------------------------------------------------------------------------------------

static void test_a_streamed_reply_parses_delta_by_delta() {
    // v0.2 changed `reply` cardinality: N deltas with final:false, closed by one final:true.
    // A parser written from the v0.1-era design would take the first frame as the whole answer.
    const char* stream[] = {"{\"type\":\"reply\",\"text\":\"При\",\"final\":false}",
                            "{\"type\":\"reply\",\"text\":\"віт\",\"final\":false}",
                            "{\"type\":\"reply\",\"text\":\"\",\"final\":true}"};

    std::string assembled;
    int deltas = 0;
    bool ended = false;
    for (const char* raw : stream) {
        const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kReply), static_cast<int>(frame.result));
        if (frame.final) {
            ended = true;
        } else {
            assembled += frame.text;
            ++deltas;
        }
    }

    TEST_ASSERT_TRUE(ended);
    TEST_ASSERT_EQUAL_INT(2, deltas);
    TEST_ASSERT_EQUAL_STRING("Привіт", assembled.c_str());
}

static void test_a_reply_missing_final_is_malformed() {
    const char* raw = "{\"type\":\"reply\",\"text\":\"hi\"}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                          static_cast<int>(parseServerFrame(raw, std::strlen(raw)).result));
}

// ---------------------------------------------------------------------------------------
// The four failure modes, each distinct
// ---------------------------------------------------------------------------------------

static void test_malformed_json_is_malformed() {
    const char* raw = "{not json at all";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                          static_cast<int>(parseServerFrame(raw, std::strlen(raw)).result));
}

static void test_a_non_object_is_malformed() {
    const char* raw = "[1,2,3]";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                          static_cast<int>(parseServerFrame(raw, std::strlen(raw)).result));
}

static void test_an_unknown_type_is_malformed() {
    const char* raw = "{\"type\":\"sing_a_song\"}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                          static_cast<int>(parseServerFrame(raw, std::strlen(raw)).result));
}

static void test_the_asr_frames_parse_and_carry_their_text() {
    const char* partial = "{\"type\":\"asr_partial\",\"text\":\"прив\"}";
    ServerFrame frame = parseServerFrame(partial, std::strlen(partial));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kAsrPartial),
                          static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_STRING("прив", frame.text.c_str());

    const char* full = "{\"type\":\"asr\",\"text\":\"Привіт.\"}";
    frame = parseServerFrame(full, std::strlen(full));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kAsr), static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_STRING("Привіт.", frame.text.c_str());
}

static void test_an_asr_frame_without_text_is_malformed() {
    const char* raw = "{\"type\":\"asr\"}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                          static_cast<int>(frame.result));
}

static void test_tts_end_parses_and_carries_nothing() {
    // Its whole job is to close the speaking window. The binary frames before it were `tts_audio`
    // because the server was speaking, not because anything labelled them -- so there is nothing
    // for this frame to carry.
    const char* raw = "{\"type\":\"tts_end\"}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kTtsEnd), static_cast<int>(frame.result));
    TEST_ASSERT_TRUE(frame.text.empty());
}

static void test_a_declared_but_unhandled_type_is_unsupported_not_malformed() {
    // The distinction the server draws too. Every not-yet-handled frame arriving at this build
    // lands here, and calling them malformed would make a working server look broken.
    //
    // `tts_end` left this list in v1.1, `asr_partial`/`asr` in v1.3, `emotion` in v2.2 and
    // `config_updated` in v2.6. A type moving out of here is what "the phase landed" looks like
    // from the parser's side, and the list shrinking to one is most of the roadmap.
    for (const char* raw : {"{\"type\":\"restart\"}"}) {
        const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kUnsupported),
                              static_cast<int>(frame.result));
        TEST_ASSERT_TRUE(!frame.type.empty());
    }
}

static void test_config_updated_carries_a_face(void) {
    const char* raw = "{\"type\":\"config_updated\",\"face_set\":\"ghost\"}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kConfigUpdated),
                          static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_STRING("ghost", frame.face_set.c_str());
}

static void test_config_updated_without_a_face_is_malformed() {
    // **Two checks, two failures.** A frame with no `face_set` is a protocol fault and is refused
    // here; a frame naming a face this build does not have is a *version disagreement* and is
    // refused by `skinIndexFor` one layer up. Only the second is worth a legible line in a log,
    // and collapsing them would lose that.
    for (const char* raw : {"{\"type\":\"config_updated\"}",
                            "{\"type\":\"config_updated\",\"face_set\":7}"}) {
        const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kMalformed),
                              static_cast<int>(frame.result));
    }
}

static void test_hello_reports_the_face_when_there_is_one() {
    const std::string with_face = roboface::buildHello("rf-1", roboface::Caps{}, 1, "flame");
    TEST_ASSERT_TRUE(with_face.find("\"face_set\":\"flame\"") != std::string::npos);

    // And omits it otherwise -- a field always present and usually meaningless teaches every
    // reader to skip it, and keeps a pre-v2.6 firmware's hello valid.
    const std::string without = roboface::buildHello("rf-1", roboface::Caps{}, 1);
    TEST_ASSERT_TRUE(without.find("face_set") == std::string::npos);
}

static void test_an_oversize_frame_is_refused_before_it_is_parsed() {
    // Mirrors the server's rule, and matters more here: 320 KB of RAM, not gigabytes.
    const std::string oversize(kMaxTextFrameBytes + 1, 'x');
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kOversize),
                          static_cast<int>(parseServerFrame(oversize).result));
}

static void test_a_frame_at_the_limit_is_still_attempted() {
    // Off-by-one guard: the boundary is inclusive on the server, so it must be here too.
    std::string at_limit = "{\"type\":\"reply\",\"text\":\"";
    at_limit.append(kMaxTextFrameBytes - at_limit.size() - std::strlen("\",\"final\":true}"), 'x');
    at_limit += "\",\"final\":true}";
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxTextFrameBytes),
                             static_cast<uint32_t>(at_limit.size()));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kReply),
                          static_cast<int>(parseServerFrame(at_limit).result));
}

// ---------------------------------------------------------------------------------------
// error and pong
// ---------------------------------------------------------------------------------------

static void test_an_error_frame_carries_its_code_and_message() {
    const char* raw = "{\"type\":\"error\",\"code\":\"proto_unsupported\",\"msg\":\"nope\"}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kError), static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kProtoUnsupported),
                          static_cast<int>(frame.code));
    TEST_ASSERT_EQUAL_STRING("nope", frame.msg.c_str());
}

static void test_an_error_with_an_unknown_code_still_parses() {
    const char* raw = "{\"type\":\"error\",\"code\":\"from_the_future\",\"msg\":\"x\"}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kError), static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kUnknown), static_cast<int>(frame.code));
}

static void test_pong_parses() {
    const char* raw = "{\"type\":\"pong\"}";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kPong),
                          static_cast<int>(parseServerFrame(raw, std::strlen(raw)).result));
}

// ---------------------------------------------------------------------------------------
// The device's own inbound bound (code review #3)
// ---------------------------------------------------------------------------------------

static void test_the_device_accepts_far_less_than_the_contract_permits() {
    // The contract's ceiling is what the *server* refuses beyond; the device's is what this board
    // can afford to allocate. 64 KiB is a fifth of 320 KB, and ArduinoJson builds a document from
    // whatever it is handed.
    TEST_ASSERT_TRUE(kMaxInboundFrameBytes < kMaxTextFrameBytes);
    TEST_ASSERT_EQUAL_UINT32(8192u, static_cast<uint32_t>(kMaxInboundFrameBytes));
}

static void test_the_device_bound_never_exceeds_the_contract() {
    // Also a static_assert in the header; asserted here too so the reason is stated where a
    // person reading the tests will meet it.
    TEST_ASSERT_TRUE(kMaxInboundFrameBytes <= kMaxTextFrameBytes);
}

static void test_a_realistic_reply_delta_is_nowhere_near_the_device_bound() {
    // The point of the smaller number: nothing this device legitimately receives comes close.
    const std::string delta = "{\"type\":\"reply\",\"text\":\"Привіт, як справи?\",\"final\":false}";

    TEST_ASSERT_TRUE(delta.size() < kMaxInboundFrameBytes / 10);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kReply),
                          static_cast<int>(parseServerFrame(delta).result));
}


// ---------------------------------------------------------------------------------------
// emotion{} -- the face channel (v2.2)
// ---------------------------------------------------------------------------------------

static void test_the_documented_emotion_frame_parses_whole() {
    // The exact object from ARCHITECTURE §EmotionFrame.
    const char* raw =
        "{\"type\":\"emotion\",\"emotion\":\"joy\",\"intensity\":0.8,"
        "\"gaze\":{\"x\":-0.4,\"y\":0.0},\"accent_color\":\"#5FFFC4\","
        "\"speaking\":true,\"ttl_ms\":6000}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kEmotion), static_cast<int>(frame.result));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(roboface::Emotion::kJoy),
                          static_cast<int>(frame.emotion.emotion));
    TEST_ASSERT_EQUAL_FLOAT(0.8f, frame.emotion.intensity);
    TEST_ASSERT_TRUE(frame.emotion.has_gaze);
    TEST_ASSERT_EQUAL_FLOAT(-0.4f, frame.emotion.gaze_x);
    TEST_ASSERT_TRUE(frame.emotion.speaking);
    TEST_ASSERT_EQUAL_UINT32(6000, frame.emotion.ttl_ms);
}

static void test_a_minimal_emotion_frame_takes_the_documented_defaults() {
    // The server omits every optional field still at its default, so this is the *common* frame
    // on the wire rather than a degenerate one. Both halves applying the same defaults is what
    // makes that omission safe.
    const char* raw = "{\"type\":\"emotion\",\"emotion\":\"sad\",\"intensity\":0.4}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(roboface::Emotion::kSad),
                          static_cast<int>(frame.emotion.emotion));
    TEST_ASSERT_FALSE(frame.emotion.has_gaze);
    TEST_ASSERT_FALSE(frame.emotion.speaking);
    TEST_ASSERT_EQUAL_UINT32(roboface::kDefaultTtlMs, frame.emotion.ttl_ms);
}

static void test_an_emotion_frame_is_never_malformed() {
    // **The one frame that coerces rather than refusing**, and the asymmetry is deliberate: a face
    // is not worth dropping a connection over, and the reply is still arriving on the same socket.
    for (const char* raw : {"{\"type\":\"emotion\"}",
                            "{\"type\":\"emotion\",\"emotion\":\"ecstatic\"}",
                            "{\"type\":\"emotion\",\"emotion\":7,\"intensity\":\"loud\"}",
                            "{\"type\":\"emotion\",\"gaze\":\"left\",\"ttl_ms\":-5}"}) {
        const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ParseResult::kEmotion),
                              static_cast<int>(frame.result));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(roboface::Emotion::kNeutral),
                              static_cast<int>(frame.emotion.emotion));
        TEST_ASSERT_TRUE(frame.emotion.intensity >= 0.0f && frame.emotion.intensity <= 1.0f);
        TEST_ASSERT_TRUE(frame.emotion.ttl_ms > 0);
    }
}

static void test_intensity_and_gaze_are_clamped() {
    const char* raw =
        "{\"type\":\"emotion\",\"emotion\":\"joy\",\"intensity\":9,"
        "\"gaze\":{\"x\":-9,\"y\":9}}";
    const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));

    TEST_ASSERT_EQUAL_FLOAT(1.0f, frame.emotion.intensity);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, frame.emotion.gaze_x);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, frame.emotion.gaze_y);
}

static void test_speaking_must_be_literally_true() {
    // A truthy value is not the flag. It gates the mouth, and `1` arriving as `true` would be a
    // decision made by a coincidence of parsing rather than by the server.
    for (const char* raw : {"{\"type\":\"emotion\",\"speaking\":1}",
                            "{\"type\":\"emotion\",\"speaking\":\"yes\"}",
                            "{\"type\":\"emotion\"}"}) {
        const ServerFrame frame = parseServerFrame(raw, std::strlen(raw));
        TEST_ASSERT_FALSE(frame.emotion.speaking);
    }
    const char* yes = "{\"type\":\"emotion\",\"speaking\":true}";
    TEST_ASSERT_TRUE(parseServerFrame(yes, std::strlen(yes)).emotion.speaking);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_device_accepts_far_less_than_the_contract_permits);
    RUN_TEST(test_the_device_bound_never_exceeds_the_contract);
    RUN_TEST(test_a_realistic_reply_delta_is_nowhere_near_the_device_bound);
    RUN_TEST(test_proto_version_is_pinned);
    RUN_TEST(test_audio_is_pcm16_16khz_mono);
    RUN_TEST(test_the_text_frame_cap_matches_the_server);
    RUN_TEST(test_device_to_server_vocabulary_is_exactly_the_contract);
    RUN_TEST(test_server_to_device_vocabulary_is_exactly_the_contract);
    RUN_TEST(test_error_codes_are_exactly_the_twelve);
    RUN_TEST(test_bad_frame_and_internal_are_distinct);
    RUN_TEST(test_every_code_round_trips_through_its_name);
    RUN_TEST(test_an_unrecognised_code_degrades_rather_than_failing);
    RUN_TEST(test_hello_is_exactly_what_the_server_accepts);
    RUN_TEST(test_the_core_s3_announces_its_real_hardware);
    RUN_TEST(test_a_wrong_proto_version_can_be_announced_deliberately);
    RUN_TEST(test_text_in_and_ping_are_what_the_server_expects);
    RUN_TEST(test_a_streamed_reply_parses_delta_by_delta);
    RUN_TEST(test_a_reply_missing_final_is_malformed);
    RUN_TEST(test_malformed_json_is_malformed);
    RUN_TEST(test_a_non_object_is_malformed);
    RUN_TEST(test_an_unknown_type_is_malformed);
    RUN_TEST(test_the_asr_frames_parse_and_carry_their_text);
    RUN_TEST(test_an_asr_frame_without_text_is_malformed);
    RUN_TEST(test_tts_end_parses_and_carries_nothing);
    RUN_TEST(test_a_declared_but_unhandled_type_is_unsupported_not_malformed);
    RUN_TEST(test_the_documented_emotion_frame_parses_whole);
    RUN_TEST(test_a_minimal_emotion_frame_takes_the_documented_defaults);
    RUN_TEST(test_an_emotion_frame_is_never_malformed);
    RUN_TEST(test_intensity_and_gaze_are_clamped);
    RUN_TEST(test_speaking_must_be_literally_true);
    RUN_TEST(test_config_updated_carries_a_face);
    RUN_TEST(test_config_updated_without_a_face_is_malformed);
    RUN_TEST(test_hello_reports_the_face_when_there_is_one);
    RUN_TEST(test_an_oversize_frame_is_refused_before_it_is_parsed);
    RUN_TEST(test_a_frame_at_the_limit_is_still_attempted);
    RUN_TEST(test_an_error_frame_carries_its_code_and_message);
    RUN_TEST(test_an_error_with_an_unknown_code_still_parses);
    RUN_TEST(test_pong_parses);
    return UNITY_END();
}

#include <unity.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "pure/vad.h"

namespace {

constexpr std::size_t kFrame = roboface::kCaptureFrameSamples;  // 320 samples = 20 ms
constexpr uint32_t kFrameMs = roboface::kCaptureFrameMs;

// A voiced frame: a tone high enough to cross zero many times per frame, which is what tells a
// voice from a thump. 300 Hz is inside the range of a speaking voice's fundamental.
std::vector<int16_t> voicedFrame(float amplitude = 0.35f, float hz = 300.0f) {
    std::vector<int16_t> frame(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
        const auto phase = 2.0f * 3.14159265f * hz * static_cast<float>(i) /
                           static_cast<float>(roboface::kCaptureSampleRate);
        frame[i] = static_cast<int16_t>(amplitude * 32767.0f * std::sin(phase));
    }
    return frame;
}

// Loud, but almost never crossing zero: the shape of the rumble that recognised as nothing at all
// in v1.3, and the reason energy alone is not enough.
std::vector<int16_t> rumbleFrame(float amplitude = 0.8f) {
    return voicedFrame(amplitude, 25.0f);
}

std::vector<int16_t> silentFrame() { return std::vector<int16_t>(kFrame, 0); }

// Feed one frame repeatedly, returning the first event that is not kNone.
roboface::VadEvent feedFor(roboface::Endpointer& vad, const std::vector<int16_t>& frame,
                           uint32_t total_ms) {
    auto seen = roboface::VadEvent::kNone;
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += kFrameMs) {
        const auto event = vad.feed(frame.data(), frame.size(), kFrameMs);
        if (event != roboface::VadEvent::kNone && seen == roboface::VadEvent::kNone) seen = event;
    }
    return seen;
}

//: Continuous speech opens exactly one window, after the minimum-speech duration and not before.
void continuous_speech_starts_one_utterance() {
    roboface::Endpointer vad;
    const auto speech = voicedFrame();
    std::size_t starts = 0;
    for (uint32_t elapsed = 0; elapsed < 2000; elapsed += kFrameMs) {
        if (vad.feed(speech.data(), speech.size(), kFrameMs) == roboface::VadEvent::kSpeechStarted) {
            ++starts;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, starts);
    TEST_ASSERT_TRUE(vad.inUtterance());
}

//: The window does not open on the first loud frame -- the minimum-speech rule must be able to
//: prevent an utterance, not only end one.
void speech_does_not_start_before_the_minimum() {
    roboface::VadSettings settings;
    settings.min_speech_ms = 200;
    roboface::Endpointer vad{settings};
    const auto speech = voicedFrame();
    // 180 ms: nine 20 ms frames, one short of the threshold.
    for (uint32_t elapsed = 0; elapsed < 180; elapsed += kFrameMs) {
        TEST_ASSERT_TRUE(vad.feed(speech.data(), speech.size(), kFrameMs) ==
                         roboface::VadEvent::kNone);
    }
    TEST_ASSERT_FALSE(vad.inUtterance());
    TEST_ASSERT_TRUE(vad.feed(speech.data(), speech.size(), kFrameMs) ==
                     roboface::VadEvent::kSpeechStarted);
}

//: Silence is silence. An hour of it must never open a window.
void silence_never_starts_an_utterance() {
    roboface::Endpointer vad;
    const auto quiet = silentFrame();
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 5000) == roboface::VadEvent::kNone);
    TEST_ASSERT_FALSE(vad.inUtterance());
}

//: A short noise burst is discarded, and -- the part that matters -- never produces a start/end
//: pair. `pure/ptt.h` refuses to call a tap an utterance for exactly this reason.
void a_short_burst_is_discarded_without_a_turn() {
    roboface::Endpointer vad;
    const auto speech = voicedFrame();
    const auto quiet = silentFrame();
    // 100 ms of noise -- well under the 250 ms minimum.
    for (uint32_t elapsed = 0; elapsed < 100; elapsed += kFrameMs) {
        TEST_ASSERT_TRUE(vad.feed(speech.data(), speech.size(), kFrameMs) ==
                         roboface::VadEvent::kNone);
    }
    const auto event = feedFor(vad, quiet, 400);
    TEST_ASSERT_TRUE(event == roboface::VadEvent::kDiscarded);
    TEST_ASSERT_FALSE(vad.inUtterance());
}

//: The end-pause closes the utterance, once, after the configured silence and not before it.
void the_end_pause_closes_the_utterance() {
    roboface::VadSettings settings;
    settings.end_pause_ms = 400;
    roboface::Endpointer vad{settings};
    const auto speech = voicedFrame();
    const auto quiet = silentFrame();
    feedFor(vad, speech, 600);
    TEST_ASSERT_TRUE(vad.inUtterance());
    // 380 ms of quiet: not yet.
    for (uint32_t elapsed = 0; elapsed < 380; elapsed += kFrameMs) {
        TEST_ASSERT_TRUE(vad.feed(quiet.data(), quiet.size(), kFrameMs) ==
                         roboface::VadEvent::kNone);
    }
    TEST_ASSERT_TRUE(vad.feed(quiet.data(), quiet.size(), kFrameMs) ==
                     roboface::VadEvent::kSpeechEnded);
    TEST_ASSERT_FALSE(vad.inUtterance());
}

//: A pause inside a sentence must not end the turn. This is the hangover doing its job.
void a_pause_inside_speech_does_not_end_the_utterance() {
    roboface::VadSettings settings;
    settings.end_pause_ms = 700;
    roboface::Endpointer vad{settings};
    const auto speech = voicedFrame();
    const auto quiet = silentFrame();
    feedFor(vad, speech, 600);
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 300) == roboface::VadEvent::kNone);
    TEST_ASSERT_TRUE(feedFor(vad, speech, 300) == roboface::VadEvent::kNone);
    TEST_ASSERT_TRUE(vad.inUtterance());
    // And the pause count restarted, so a full end-pause is still required afterwards.
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 720) == roboface::VadEvent::kSpeechEnded);
}

//: Loud low-frequency rumble is not speech, however loud it is. The captures that cost v1.3 an
//: evening measured a healthy level and carried no voice; energy alone would have opened a window
//: on every one of them.
void rumble_is_loud_but_is_not_a_voice() {
    roboface::Endpointer vad;
    const auto rumble = rumbleFrame();
    TEST_ASSERT_TRUE(roboface::peakLevel(rumble.data(), rumble.size()) > 0.5f);
    TEST_ASSERT_TRUE(feedFor(vad, rumble, 3000) == roboface::VadEvent::kNone);
    TEST_ASSERT_FALSE(vad.inUtterance());
}

//: The two measures are independent: quiet speech is rejected on level, loud rumble on crossings.
void a_frame_needs_both_level_and_crossings() {
    roboface::VadSettings settings;
    const auto loud_voice = voicedFrame(0.35f, 300.0f);
    const auto quiet_voice = voicedFrame(0.01f, 300.0f);
    const auto loud_rumble = rumbleFrame(0.8f);
    TEST_ASSERT_TRUE(roboface::frameIsVoiced(loud_voice.data(), loud_voice.size(), settings));
    TEST_ASSERT_FALSE(roboface::frameIsVoiced(quiet_voice.data(), quiet_voice.size(), settings));
    TEST_ASSERT_FALSE(roboface::frameIsVoiced(loud_rumble.data(), loud_rumble.size(), settings));
}

//: Zero counts as neither sign, so a padded gap does not read as a crossing on the way in or out.
void zeros_are_not_crossings() {
    std::vector<int16_t> padded(kFrame, 0);
    TEST_ASSERT_EQUAL_UINT32(0, roboface::zeroCrossings(padded.data(), padded.size()));
    const int16_t alternating[] = {100, 0, 0, -100, 0, 100};
    TEST_ASSERT_EQUAL_UINT32(2, roboface::zeroCrossings(alternating, 6));
    TEST_ASSERT_EQUAL_UINT32(0, roboface::zeroCrossings(nullptr, 4));
}

//: `reset` clears the utterance without touching the settings -- what half-duplex needs when
//: listening resumes after the device has been speaking.
void reset_clears_the_utterance_but_keeps_the_settings() {
    roboface::VadSettings settings;
    settings.end_pause_ms = 999;
    roboface::Endpointer vad{settings};
    const auto speech = voicedFrame();
    feedFor(vad, speech, 600);
    TEST_ASSERT_TRUE(vad.inUtterance());
    vad.reset();
    TEST_ASSERT_FALSE(vad.inUtterance());
    TEST_ASSERT_EQUAL_UINT32(999, vad.settings().end_pause_ms);
}

//: Two utterances in a row each report their own start and end -- the counters must not carry over.
void a_second_utterance_reports_its_own_transitions() {
    roboface::Endpointer vad;
    const auto speech = voicedFrame();
    const auto quiet = silentFrame();
    TEST_ASSERT_TRUE(feedFor(vad, speech, 600) == roboface::VadEvent::kSpeechStarted);
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 800) == roboface::VadEvent::kSpeechEnded);
    TEST_ASSERT_TRUE(feedFor(vad, speech, 600) == roboface::VadEvent::kSpeechStarted);
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 800) == roboface::VadEvent::kSpeechEnded);
}

//: Pre-roll is rounded **up** to whole frames: a ring one frame short would clip the first syllable,
//: which is the one thing pre-roll exists to protect.
void pre_roll_rounds_up_to_whole_frames() {
    TEST_ASSERT_EQUAL_UINT32(15, roboface::preRollFrames(300));
    TEST_ASSERT_EQUAL_UINT32(5, roboface::preRollFrames(90));   // 4.5 frames -> 5
    TEST_ASSERT_EQUAL_UINT32(1, roboface::preRollFrames(1));
    TEST_ASSERT_EQUAL_UINT32(0, roboface::preRollFrames(0));
}

//: A value inside the band is accepted and reported back the way it was set.
void a_sensible_setting_is_accepted() {
    roboface::VadSettings settings;
    TEST_ASSERT_TRUE(roboface::setSensitivityPct(settings, 12));
    TEST_ASSERT_EQUAL_INT(12, roboface::sensitivityPct(settings));
    TEST_ASSERT_TRUE(roboface::setEndPauseMs(settings, 900));
    TEST_ASSERT_EQUAL_UINT32(900, settings.end_pause_ms);
}

//: An out-of-range value is **refused**, and leaves the setting untouched. Clamping would answer
//: "set it to zero" with "done" and leave the person wondering why nothing changed.
void an_out_of_range_setting_is_refused_not_clamped() {
    roboface::VadSettings settings;
    const float original = settings.sensitivity;
    TEST_ASSERT_FALSE(roboface::setSensitivityPct(settings, 0));
    TEST_ASSERT_FALSE(roboface::setSensitivityPct(settings, 200));
    TEST_ASSERT_FALSE(roboface::setSensitivityPct(settings, -5));
    TEST_ASSERT_TRUE(settings.sensitivity == original);

    const uint32_t pause = settings.end_pause_ms;
    TEST_ASSERT_FALSE(roboface::setEndPauseMs(settings, 10));
    TEST_ASSERT_FALSE(roboface::setEndPauseMs(settings, 60000));
    TEST_ASSERT_EQUAL_UINT32(pause, settings.end_pause_ms);
}

//: The band's own edges are inside it -- an inclusive range stated inclusively.
void the_bounds_themselves_are_allowed() {
    roboface::VadSettings settings;
    TEST_ASSERT_TRUE(roboface::setSensitivityPct(settings, roboface::kVadMinSensitivityPct));
    TEST_ASSERT_TRUE(roboface::setSensitivityPct(settings, roboface::kVadMaxSensitivityPct));
    TEST_ASSERT_TRUE(roboface::setEndPauseMs(settings, roboface::kVadMinEndPauseMs));
    TEST_ASSERT_TRUE(roboface::setEndPauseMs(settings, roboface::kVadMaxEndPauseMs));
}

//: A live endpointer takes new settings without being rebuilt, so tuning does not lose the
//: utterance in progress or need a reboot.
void configure_replaces_the_settings_in_place() {
    roboface::VadSettings settings;
    roboface::setEndPauseMs(settings, 1200);
    roboface::Endpointer vad;
    vad.configure(settings);
    TEST_ASSERT_EQUAL_UINT32(1200, vad.settings().end_pause_ms);
}

//: An utterance in progress can always be finished, whatever happens to the feature switch. The
//: endpointer is the half that must keep working: a window opened before active listening was
//: turned off still has to report its end, or the caller has nothing to close it with.
void an_utterance_can_always_be_finished() {
    roboface::Endpointer vad;
    const auto speech = voicedFrame();
    const auto quiet = silentFrame();
    TEST_ASSERT_TRUE(feedFor(vad, speech, 600) == roboface::VadEvent::kSpeechStarted);
    // Whatever the caller does with its own switch, the end of this utterance is still reported.
    TEST_ASSERT_TRUE(feedFor(vad, quiet, 800) == roboface::VadEvent::kSpeechEnded);
    TEST_ASSERT_FALSE(vad.inUtterance());
}

}  // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(continuous_speech_starts_one_utterance);
    RUN_TEST(speech_does_not_start_before_the_minimum);
    RUN_TEST(silence_never_starts_an_utterance);
    RUN_TEST(a_short_burst_is_discarded_without_a_turn);
    RUN_TEST(the_end_pause_closes_the_utterance);
    RUN_TEST(a_pause_inside_speech_does_not_end_the_utterance);
    RUN_TEST(rumble_is_loud_but_is_not_a_voice);
    RUN_TEST(a_frame_needs_both_level_and_crossings);
    RUN_TEST(zeros_are_not_crossings);
    RUN_TEST(reset_clears_the_utterance_but_keeps_the_settings);
    RUN_TEST(a_second_utterance_reports_its_own_transitions);
    RUN_TEST(pre_roll_rounds_up_to_whole_frames);
    RUN_TEST(a_sensible_setting_is_accepted);
    RUN_TEST(an_out_of_range_setting_is_refused_not_clamped);
    RUN_TEST(the_bounds_themselves_are_allowed);
    RUN_TEST(configure_replaces_the_settings_in_place);
    RUN_TEST(an_utterance_can_always_be_finished);
    return UNITY_END();
}

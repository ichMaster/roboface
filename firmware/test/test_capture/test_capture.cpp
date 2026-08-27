// Host tests for the capture arithmetic.
//
// None of this can crash; all of it can be quietly wrong. A frame size that is off by a factor
// produces audio that plays at the wrong speed, and a duration computed from a clock rather than
// from bytes drifts -- neither announces itself.

#include <unity.h>

#include "pure/capture.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

void test_the_frame_is_twenty_milliseconds_at_sixteen_kilohertz() {
    TEST_ASSERT_EQUAL_UINT32(320, kCaptureFrameSamples);
    TEST_ASSERT_EQUAL_UINT32(640, kCaptureFrameBytes);
}

void test_the_frame_sits_in_the_roadmaps_twenty_to_forty_millisecond_band() {
    const uint32_t ms = msForBytes(kCaptureFrameBytes);
    TEST_ASSERT_TRUE(ms >= 20);
    TEST_ASSERT_TRUE(ms <= 40);
}

void test_samples_for_ms_truncates_rather_than_rounding_up() {
    // A frame that claimed more samples than it holds would read past its buffer. Slightly short
    // is merely slightly short.
    TEST_ASSERT_EQUAL_UINT32(320, samplesForMs(20));
    TEST_ASSERT_EQUAL_UINT32(16, samplesForMs(1));
    TEST_ASSERT_EQUAL_UINT32(0, samplesForMs(0));
    TEST_ASSERT_EQUAL_UINT32(0, samplesForMs(-5));
}

void test_duration_comes_from_bytes_not_from_a_clock() {
    TEST_ASSERT_EQUAL_UINT32(1000, msForBytes(kCaptureSampleRate * 2));
    TEST_ASSERT_EQUAL_UINT32(20, msForBytes(kCaptureFrameBytes));
    TEST_ASSERT_EQUAL_UINT32(0, msForBytes(0));
}

void test_a_bad_rate_does_not_divide_by_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, msForBytes(1000, 0));
    TEST_ASSERT_EQUAL_UINT32(0, samplesForMs(20, 0));
}

void test_a_new_tally_is_empty() {
    const CaptureTally tally;
    TEST_ASSERT_TRUE(tally.empty());
    TEST_ASSERT_EQUAL_UINT32(0, tally.frames());
    TEST_ASSERT_EQUAL_UINT32(0, tally.durationMs());
}

void test_the_tally_counts_what_was_sent() {
    CaptureTally tally;
    for (int i = 0; i < 50; ++i) tally.recordFrame(kCaptureFrameBytes);

    TEST_ASSERT_EQUAL_UINT32(50, tally.frames());
    TEST_ASSERT_EQUAL_UINT32(50 * kCaptureFrameBytes, tally.bytes());
    // Fifty 20 ms frames is one second, and it is derived from the bytes rather than timed.
    TEST_ASSERT_EQUAL_UINT32(1000, tally.durationMs());
    TEST_ASSERT_FALSE(tally.empty());
}

void test_reset_clears_it_between_utterances() {
    // Without this the second utterance reports the first's length, and the DoD's evidence --
    // frames already sent when the window closes -- would be true even for an empty one.
    CaptureTally tally;
    tally.recordFrame(kCaptureFrameBytes);
    tally.reset();
    TEST_ASSERT_TRUE(tally.empty());
    TEST_ASSERT_EQUAL_UINT32(0, tally.bytes());
}

void test_a_short_final_frame_is_counted_as_what_it_holds() {
    // The last frame of an utterance is whatever was captured when the button came up.
    CaptureTally tally;
    tally.recordFrame(kCaptureFrameBytes);
    tally.recordFrame(160);
    TEST_ASSERT_EQUAL_UINT32(2, tally.frames());
    TEST_ASSERT_EQUAL_UINT32(kCaptureFrameBytes + 160, tally.bytes());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_frame_is_twenty_milliseconds_at_sixteen_kilohertz);
    RUN_TEST(test_the_frame_sits_in_the_roadmaps_twenty_to_forty_millisecond_band);
    RUN_TEST(test_samples_for_ms_truncates_rather_than_rounding_up);
    RUN_TEST(test_duration_comes_from_bytes_not_from_a_clock);
    RUN_TEST(test_a_bad_rate_does_not_divide_by_zero);
    RUN_TEST(test_a_new_tally_is_empty);
    RUN_TEST(test_the_tally_counts_what_was_sent);
    RUN_TEST(test_reset_clears_it_between_utterances);
    RUN_TEST(test_a_short_final_frame_is_counted_as_what_it_holds);
    return UNITY_END();
}

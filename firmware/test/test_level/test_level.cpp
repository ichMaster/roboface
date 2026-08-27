// Host tests for the input-level envelope.
//
// None of this is visible enough to check by eye on a 28 px band: a meter that read half-scale for
// silence would still look like a meter.

#include <unity.h>

#include <vector>

#include "pure/level.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

void test_silence_reads_as_zero() {
    const std::vector<int16_t> silence(320, 0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peakLevel(silence.data(), silence.size()));
}

void test_an_empty_frame_reads_as_zero_rather_than_dividing() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peakLevel(nullptr, 0));
    const std::vector<int16_t> none;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, peakLevel(none.data(), 0));
}

void test_full_scale_reads_as_one() {
    const std::vector<int16_t> loud(320, 32767);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, peakLevel(loud.data(), loud.size()));
}

void test_the_most_negative_sample_does_not_overflow() {
    // INT16_MIN has no positive counterpart; abs() on it is undefined and would read as a huge
    // level or a negative one.
    const std::vector<int16_t> loud(8, -32768);
    const float level = peakLevel(loud.data(), loud.size());
    TEST_ASSERT_TRUE(level > 0.99f);
    TEST_ASSERT_TRUE(level <= 1.0f);
}

void test_the_peak_is_the_loudest_sample_not_the_average() {
    // A frame that is mostly quiet with one loud sample is a frame where something happened.
    std::vector<int16_t> frame(320, 0);
    frame[100] = 16384;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, peakLevel(frame.data(), frame.size()));
}

void test_a_rise_is_followed_immediately() {
    // A meter that eased into a rise would under-report the start of every word.
    TEST_ASSERT_EQUAL_FLOAT(0.9f, decayToward(0.1f, 0.9f));
}

void test_a_fall_is_eased() {
    const float eased = decayToward(1.0f, 0.0f);
    TEST_ASSERT_TRUE(eased > 0.0f);
    TEST_ASSERT_TRUE(eased < 1.0f);
}

void test_a_fall_reaches_zero_eventually() {
    // Eased, not stuck: a meter that never returned to zero would claim the device is still
    // hearing something after the room went quiet.
    float level = 1.0f;
    for (int i = 0; i < 100; ++i) level = decayToward(level, 0.0f);
    TEST_ASSERT_TRUE(level < 0.01f);
}

void test_bars_are_none_at_silence_and_all_at_full() {
    TEST_ASSERT_EQUAL_UINT32(0, barsForLevel(0.0f, 10));
    TEST_ASSERT_EQUAL_UINT32(10, barsForLevel(1.0f, 10));
    TEST_ASSERT_EQUAL_UINT32(10, barsForLevel(2.0f, 10));
    TEST_ASSERT_EQUAL_UINT32(0, barsForLevel(-1.0f, 10));
}

void test_bars_never_exceed_the_total() {
    for (int step = 0; step <= 100; ++step) {
        const float level = static_cast<float>(step) / 100.0f;
        TEST_ASSERT_TRUE(barsForLevel(level, 12) <= 12);
    }
}

void test_bars_rise_monotonically_with_level() {
    std::size_t previous = 0;
    for (int step = 0; step <= 100; ++step) {
        const std::size_t bars = barsForLevel(static_cast<float>(step) / 100.0f, 12);
        TEST_ASSERT_TRUE(bars >= previous);
        previous = bars;
    }
}

void test_zero_bars_requested_is_zero_not_a_division() {
    TEST_ASSERT_EQUAL_UINT32(0, barsForLevel(1.0f, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_silence_reads_as_zero);
    RUN_TEST(test_an_empty_frame_reads_as_zero_rather_than_dividing);
    RUN_TEST(test_full_scale_reads_as_one);
    RUN_TEST(test_the_most_negative_sample_does_not_overflow);
    RUN_TEST(test_the_peak_is_the_loudest_sample_not_the_average);
    RUN_TEST(test_a_rise_is_followed_immediately);
    RUN_TEST(test_a_fall_is_eased);
    RUN_TEST(test_a_fall_reaches_zero_eventually);
    RUN_TEST(test_bars_are_none_at_silence_and_all_at_full);
    RUN_TEST(test_bars_never_exceed_the_total);
    RUN_TEST(test_bars_rise_monotonically_with_level);
    RUN_TEST(test_zero_bars_requested_is_zero_not_a_division);
    return UNITY_END();
}

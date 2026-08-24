// Host tests for the playback buffer.
//
// Both properties under test produce *audible* faults rather than crashes, which is exactly why
// they are asserted here: a dropped tail sounds like a clipped word, and a zero-filled underrun
// sounds like the sentence ended.

#include <unity.h>

#include <cstring>
#include <vector>

#include "pure/ring_buffer.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> ramp(std::size_t n, uint8_t start = 0) {
    std::vector<uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(start + i);
    return out;
}

void test_a_new_buffer_is_empty() {
    RingBuffer<64> buffer;
    TEST_ASSERT_TRUE(buffer.empty());
    TEST_ASSERT_EQUAL_UINT32(0, buffer.size());
    TEST_ASSERT_EQUAL_UINT32(64, buffer.free());
}

void test_what_goes_in_comes_out_unchanged() {
    RingBuffer<64> buffer;
    const std::vector<uint8_t> in = ramp(32);
    TEST_ASSERT_EQUAL_UINT32(32, buffer.write(in.data(), in.size()));

    std::vector<uint8_t> out(32);
    TEST_ASSERT_EQUAL_UINT32(32, buffer.read(out.data(), out.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), out.data(), 32);
    TEST_ASSERT_TRUE(buffer.empty());
}

void test_a_full_buffer_accepts_what_fits_and_reports_it() {
    // The short return is backpressure, not an error. Silently dropping the rest would lose a few
    // milliseconds of speech exactly when the network is struggling.
    RingBuffer<16> buffer;
    const std::vector<uint8_t> in = ramp(24);
    TEST_ASSERT_EQUAL_UINT32(16, buffer.write(in.data(), in.size()));
    TEST_ASSERT_TRUE(buffer.full());
    TEST_ASSERT_EQUAL_UINT32(0, buffer.write(in.data(), 1));
}

void test_nothing_is_lost_when_the_caller_honours_backpressure() {
    // The lossless property, stated as the caller actually uses it: offer the remainder again
    // after draining, and every byte arrives exactly once, in order.
    RingBuffer<16> buffer;
    const std::vector<uint8_t> in = ramp(100);
    std::vector<uint8_t> received;

    std::size_t offset = 0;
    while (offset < in.size() || !buffer.empty()) {
        if (offset < in.size()) offset += buffer.write(in.data() + offset, in.size() - offset);
        uint8_t sink[7];
        const std::size_t got = buffer.read(sink, sizeof(sink));
        received.insert(received.end(), sink, sink + got);
    }
    TEST_ASSERT_EQUAL_UINT32(in.size(), received.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), received.data(), in.size());
}

void test_it_wraps_without_corrupting() {
    RingBuffer<8> buffer;
    const std::vector<uint8_t> first = ramp(6, 1);
    buffer.write(first.data(), first.size());

    uint8_t sink[4];
    buffer.read(sink, 4);  // head moves to 4

    const std::vector<uint8_t> second = ramp(5, 100);
    TEST_ASSERT_EQUAL_UINT32(5, buffer.write(second.data(), second.size()));

    std::vector<uint8_t> out(7);
    TEST_ASSERT_EQUAL_UINT32(7, buffer.read(out.data(), out.size()));
    const uint8_t expected[7] = {5, 6, 100, 101, 102, 103, 104};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out.data(), 7);
}

void test_an_underrun_returns_zero_rather_than_silence() {
    // The caller waits. Filling the gap with zeros would put silence in the middle of a word,
    // which does not sound like a stall -- it sounds like the sentence ended.
    RingBuffer<64> buffer;
    uint8_t sink[16];
    TEST_ASSERT_EQUAL_UINT32(0, buffer.read(sink, sizeof(sink)));
}

void test_a_partial_underrun_gives_what_there_is() {
    RingBuffer<64> buffer;
    const std::vector<uint8_t> in = ramp(5);
    buffer.write(in.data(), in.size());

    uint8_t sink[16];
    TEST_ASSERT_EQUAL_UINT32(5, buffer.read(sink, sizeof(sink)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), sink, 5);
}

void test_sample_reads_never_split_a_pcm16_sample() {
    // An odd byte count handed to the speaker shifts every following sample by a byte: the rest of
    // the phrase becomes noise rather than producing one click.
    RingBuffer<64> buffer;
    const std::vector<uint8_t> in = ramp(7);
    buffer.write(in.data(), in.size());

    uint8_t sink[16];
    const std::size_t got = buffer.readSamples(sink, sizeof(sink));
    TEST_ASSERT_EQUAL_UINT32(6, got);
    TEST_ASSERT_EQUAL_UINT32(0, got % 2);
    TEST_ASSERT_EQUAL_UINT32(1, buffer.size());  // the odd byte waits for its partner
}

void test_clear_empties_it() {
    RingBuffer<32> buffer;
    const std::vector<uint8_t> in = ramp(10);
    buffer.write(in.data(), in.size());
    buffer.clear();
    TEST_ASSERT_TRUE(buffer.empty());
    TEST_ASSERT_EQUAL_UINT32(32, buffer.free());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_new_buffer_is_empty);
    RUN_TEST(test_what_goes_in_comes_out_unchanged);
    RUN_TEST(test_a_full_buffer_accepts_what_fits_and_reports_it);
    RUN_TEST(test_nothing_is_lost_when_the_caller_honours_backpressure);
    RUN_TEST(test_it_wraps_without_corrupting);
    RUN_TEST(test_an_underrun_returns_zero_rather_than_silence);
    RUN_TEST(test_a_partial_underrun_gives_what_there_is);
    RUN_TEST(test_sample_reads_never_split_a_pcm16_sample);
    RUN_TEST(test_clear_empties_it);
    return UNITY_END();
}

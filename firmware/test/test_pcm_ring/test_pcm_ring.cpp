// Host tests for the playback backlog.
//
// The property under test is losslessness, and it is worth stating why it is not academic: the
// buffer exists because `playRaw` drops a chunk when its slots are full rather than waiting. If
// this buffer also dropped, the two losses would compound and the audible result -- a reply that
// starts cleanly and then breaks up -- would be identical, which is exactly the fault this
// replaced.

#include <unity.h>

#include <vector>

#include "pure/pcm_ring.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> ramp(std::size_t n, uint8_t start = 0) {
    std::vector<uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(start + i);
    return out;
}

void test_an_unattached_ring_reports_itself() {
    PcmRing ring;
    TEST_ASSERT_FALSE(ring.attached());
    TEST_ASSERT_TRUE(ring.empty());
}

void test_what_goes_in_comes_out_unchanged() {
    uint8_t storage[64];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> in = ramp(32);
    TEST_ASSERT_EQUAL_UINT32(32, ring.write(in.data(), in.size()));

    std::vector<uint8_t> out(32);
    TEST_ASSERT_EQUAL_UINT32(32, ring.readSamples(out.data(), out.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), out.data(), 32);
    TEST_ASSERT_TRUE(ring.empty());
}

void test_a_full_ring_reports_what_it_took() {
    uint8_t storage[16];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> in = ramp(24);
    TEST_ASSERT_EQUAL_UINT32(16, ring.write(in.data(), in.size()));
    TEST_ASSERT_EQUAL_UINT32(0, ring.write(in.data(), 1));
}

void test_nothing_is_lost_when_the_caller_honours_backpressure() {
    uint8_t storage[16];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> in = ramp(200);
    std::vector<uint8_t> received;
    std::size_t offset = 0;
    while (offset < in.size() || !ring.empty()) {
        if (offset < in.size()) offset += ring.write(in.data() + offset, in.size() - offset);
        uint8_t sink[6];
        const std::size_t got = ring.readSamples(sink, sizeof(sink));
        received.insert(received.end(), sink, sink + got);
    }
    TEST_ASSERT_EQUAL_UINT32(in.size(), received.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), received.data(), in.size());
}

void test_it_wraps_without_corrupting() {
    uint8_t storage[8];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> first = ramp(6, 1);
    ring.write(first.data(), first.size());
    uint8_t sink[4];
    ring.readSamples(sink, 4);

    const std::vector<uint8_t> second = ramp(6, 100);
    TEST_ASSERT_EQUAL_UINT32(6, ring.write(second.data(), second.size()));

    std::vector<uint8_t> out(8);
    TEST_ASSERT_EQUAL_UINT32(8, ring.readSamples(out.data(), out.size()));
    const uint8_t expected[8] = {5, 6, 100, 101, 102, 103, 104, 105};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out.data(), 8);
}

void test_reads_never_split_a_pcm16_sample() {
    uint8_t storage[64];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> in = ramp(7);
    ring.write(in.data(), in.size());

    uint8_t sink[16];
    TEST_ASSERT_EQUAL_UINT32(6, ring.readSamples(sink, sizeof(sink)));
    TEST_ASSERT_EQUAL_UINT32(1, ring.size());  // the odd byte waits for its partner
}

void test_an_empty_ring_gives_nothing_rather_than_silence() {
    uint8_t storage[32];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));
    uint8_t sink[8];
    TEST_ASSERT_EQUAL_UINT32(0, ring.readSamples(sink, sizeof(sink)));
}

void test_unread_puts_a_refused_chunk_back_at_the_front() {
    // The speaker refuses a buffer when both slots are claimed. The chunk must stay *next*, not go
    // to the back -- appending it would reorder the reply, which is worse than the pause.
    uint8_t storage[32];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> in = ramp(12, 1);
    ring.write(in.data(), in.size());

    uint8_t taken[4];
    TEST_ASSERT_EQUAL_UINT32(4, ring.readSamples(taken, sizeof(taken)));
    ring.unread(4);
    TEST_ASSERT_EQUAL_UINT32(12, ring.size());

    std::vector<uint8_t> out(12);
    TEST_ASSERT_EQUAL_UINT32(12, ring.readSamples(out.data(), out.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data(), out.data(), 12);
}

void test_unread_survives_a_wrap() {
    uint8_t storage[8];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));

    const std::vector<uint8_t> first = ramp(6, 1);
    ring.write(first.data(), first.size());
    uint8_t sink[6];
    ring.readSamples(sink, 6);              // head is now at 6
    const std::vector<uint8_t> second = ramp(4, 50);
    ring.write(second.data(), second.size());  // wraps

    uint8_t taken[2];
    ring.readSamples(taken, 2);
    ring.unread(2);

    std::vector<uint8_t> out(4);
    TEST_ASSERT_EQUAL_UINT32(4, ring.readSamples(out.data(), out.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second.data(), out.data(), 4);
}

void test_clear_empties_it() {
    uint8_t storage[32];
    PcmRing ring;
    ring.attach(storage, sizeof(storage));
    const std::vector<uint8_t> in = ramp(10);
    ring.write(in.data(), in.size());
    ring.clear();
    TEST_ASSERT_TRUE(ring.empty());
    TEST_ASSERT_EQUAL_UINT32(32, ring.free());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_an_unattached_ring_reports_itself);
    RUN_TEST(test_what_goes_in_comes_out_unchanged);
    RUN_TEST(test_a_full_ring_reports_what_it_took);
    RUN_TEST(test_nothing_is_lost_when_the_caller_honours_backpressure);
    RUN_TEST(test_it_wraps_without_corrupting);
    RUN_TEST(test_reads_never_split_a_pcm16_sample);
    RUN_TEST(test_an_empty_ring_gives_nothing_rather_than_silence);
    RUN_TEST(test_unread_puts_a_refused_chunk_back_at_the_front);
    RUN_TEST(test_unread_survives_a_wrap);
    RUN_TEST(test_clear_empties_it);
    return UNITY_END();
}

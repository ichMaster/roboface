#include <unity.h>

#include "pure/timed_capture.h"

namespace {

constexpr std::uint32_t kRate = 16000;
constexpr std::size_t kChunk = 320;  // 20 ms

//: The whole point: a buffer that was queued a microsecond ago is not readable, however eagerly
//: the recorder accepted it.
void queued_is_not_the_same_as_filled() {
    roboface::TimedCapture capture{kRate};
    capture.start(1000);
    capture.queued(kChunk * 2);
    TEST_ASSERT_EQUAL_UINT32(0, capture.filled(1000));
    TEST_ASSERT_FALSE(capture.readable(1000, kChunk));
}

//: After one chunk's worth of time, exactly one chunk is readable.
void time_releases_one_chunk_at_a_time() {
    roboface::TimedCapture capture{kRate};
    capture.start(0);
    capture.queued(kChunk * 4);
    TEST_ASSERT_TRUE(capture.readable(20, kChunk));
    capture.sent(kChunk);
    TEST_ASSERT_FALSE(capture.readable(20, kChunk));
    TEST_ASSERT_TRUE(capture.readable(40, kChunk));
}

//: The clock never authorises reading past what was actually handed to the recorder.
void the_clock_cannot_outrun_what_was_queued() {
    roboface::TimedCapture capture{kRate};
    capture.start(0);
    capture.queued(kChunk);
    TEST_ASSERT_EQUAL_UINT32(kChunk, capture.filled(5000));
    capture.sent(kChunk);
    TEST_ASSERT_FALSE(capture.readable(5000, kChunk));
}

//: A window that runs a while releases audio at the capture rate, not faster.
void released_audio_tracks_real_time() {
    roboface::TimedCapture capture{kRate};
    capture.start(0);
    capture.queued(kRate * 4);  // four seconds queued up front
    TEST_ASSERT_EQUAL_UINT32(kRate, capture.filled(1000));
    TEST_ASSERT_EQUAL_UINT32(kRate * 2, capture.filled(2000));
}

//: `pending` is what the ring has to hold: queued and not yet read out.
void pending_is_what_the_ring_must_hold() {
    roboface::TimedCapture capture{kRate};
    capture.start(0);
    capture.queued(kChunk * 3);
    capture.sent(kChunk);
    TEST_ASSERT_EQUAL_UINT32(kChunk * 2, capture.pending());
}

//: Starting a second window forgets the first one entirely.
void a_new_window_starts_from_nothing() {
    roboface::TimedCapture capture{kRate};
    capture.start(0);
    capture.queued(kRate);
    capture.sent(kRate);
    capture.start(9000);
    TEST_ASSERT_EQUAL_UINT32(0, capture.queuedSamples());
    TEST_ASSERT_EQUAL_UINT32(0, capture.sentSamples());
    TEST_ASSERT_EQUAL_UINT32(0, capture.filled(9000));
}

//: `millis()` wraps every 49 days; the arithmetic must survive it rather than releasing the whole
//: ring at once.
void the_millisecond_counter_may_wrap() {
    roboface::TimedCapture capture{kRate};
    const std::uint32_t near_wrap = 0xFFFFFFFFu - 10u;
    capture.start(near_wrap);
    capture.queued(kChunk * 4);
    TEST_ASSERT_TRUE(capture.readable(near_wrap + 30u, kChunk));
    TEST_ASSERT_EQUAL_UINT32(480, capture.filled(near_wrap + 30u));
}

}  // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(queued_is_not_the_same_as_filled);
    RUN_TEST(time_releases_one_chunk_at_a_time);
    RUN_TEST(the_clock_cannot_outrun_what_was_queued);
    RUN_TEST(released_audio_tracks_real_time);
    RUN_TEST(pending_is_what_the_ring_must_hold);
    RUN_TEST(a_new_window_starts_from_nothing);
    RUN_TEST(the_millisecond_counter_may_wrap);
    return UNITY_END();
}

#include <unity.h>

#include <vector>

#include "pure/idle.h"

namespace {

constexpr uint32_t kFrameMs = 50;  // ~20 FPS, the rate v2.1 targets

// Run the loop for `total_ms` and collect the moment each blink began.
std::vector<uint32_t> blinkStarts(roboface::IdleLoop& loop, uint32_t total_ms) {
    std::vector<uint32_t> starts;
    bool was_blinking = false;
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += kFrameMs) {
        loop.advance(kFrameMs);
        if (loop.isBlinking() && !was_blinking) starts.push_back(loop.elapsedMs());
        was_blinking = loop.isBlinking();
    }
    return starts;
}

//: The same seed and the same timeline produce the same blinks. Without this the loop could only
//: be checked by watching a device, and "it looked about right" is not a test.
void the_same_seed_gives_the_same_sequence() {
    roboface::IdleLoop first{1234};
    roboface::IdleLoop second{1234};
    TEST_ASSERT_TRUE(blinkStarts(first, 60000) == blinkStarts(second, 60000));
}

//: Different seeds do not. A generator that ignored its seed would pass the test above and be
//: exactly as periodic as no generator at all.
void different_seeds_give_different_sequences() {
    roboface::IdleLoop first{1234};
    roboface::IdleLoop second{9876};
    TEST_ASSERT_FALSE(blinkStarts(first, 60000) == blinkStarts(second, 60000));
}

//: Intervals stay inside the band. A blink every eight seconds looks switched off; one every
//: second looks nervous.
void intervals_stay_within_the_band() {
    roboface::IdleLoop loop{7};
    const auto starts = blinkStarts(loop, 300000);  // five minutes
    TEST_ASSERT_TRUE(starts.size() > 40);
    for (std::size_t i = 1; i < starts.size(); ++i) {
        const uint32_t gap = starts[i] - starts[i - 1];
        // The gap contains one blink's own duration plus the drawn interval, and the frame
        // quantisation adds up to one frame either side.
        TEST_ASSERT_TRUE(gap >= roboface::kBlinkMinIntervalMs);
        TEST_ASSERT_TRUE(gap <= roboface::kBlinkMaxIntervalMs + roboface::kBlinkDurationMs +
                                    2 * kFrameMs);
    }
}

//: A blink is brief and complete -- it does not stick, and it does not last a frame.
void a_blink_completes_in_its_duration() {
    roboface::IdleLoop loop{42};
    uint32_t closed_ms = 0;
    bool seen = false;
    for (uint32_t elapsed = 0; elapsed < 30000; elapsed += 10) {
        loop.advance(10);
        if (loop.isBlinking()) {
            closed_ms += 10;
            seen = true;
        } else if (seen) {
            break;
        }
    }
    TEST_ASSERT_TRUE(seen);
    TEST_ASSERT_TRUE(closed_ms >= roboface::kBlinkDurationMs);
    TEST_ASSERT_TRUE(closed_ms <= roboface::kBlinkDurationMs + 20);
}

//: Intensity 0 stills the face without stopping the loop -- it keeps its schedule, it simply does
//: not move. That distinction is what lets v2.2 damp the idle during a turn and resume it after,
//: rather than restarting a blink cycle every time.
void intensity_zero_stills_without_stopping() {
    roboface::IdleLoop loop{3};
    loop.setIntensity(0.0f);
    for (uint32_t elapsed = 0; elapsed < 20000; elapsed += kFrameMs) {
        const auto offsets = loop.advance(kFrameMs);
        TEST_ASSERT_EQUAL_FLOAT(1.0f, offsets.eye_scale);
        TEST_ASSERT_TRUE(offsets.bob_y == 0.0f);
        TEST_ASSERT_TRUE(offsets.gaze_x == 0.0f);
    }
    TEST_ASSERT_TRUE(loop.elapsedMs() >= 20000);
}

//: And full intensity closes the eyes completely during a blink -- a partial blink at this size
//: reads as a rendering fault rather than as a face.
void a_blink_closes_the_eyes_completely() {
    roboface::IdleLoop loop{11};
    bool saw_closed = false;
    for (uint32_t elapsed = 0; elapsed < 30000; elapsed += 10) {
        const auto offsets = loop.advance(10);
        if (loop.isBlinking()) {
            TEST_ASSERT_EQUAL_FLOAT(0.0f, offsets.eye_scale);
            saw_closed = true;
        }
    }
    TEST_ASSERT_TRUE(saw_closed);
}

//: The breath and the drift stay inside their amplitudes, always. An overshoot would push the
//: features outside the face, which is a very visible way to fail.
void motion_stays_within_its_amplitudes() {
    roboface::IdleLoop loop{5};
    for (uint32_t elapsed = 0; elapsed < 120000; elapsed += 10) {
        const auto offsets = loop.advance(10);
        TEST_ASSERT_TRUE(offsets.bob_y <= roboface::kBreathAmplitudePx + 0.01f);
        TEST_ASSERT_TRUE(offsets.bob_y >= -roboface::kBreathAmplitudePx - 0.01f);
        TEST_ASSERT_TRUE(offsets.gaze_x <= roboface::kGazeAmplitudePx + 0.01f);
        TEST_ASSERT_TRUE(offsets.gaze_x >= -roboface::kGazeAmplitudePx - 0.01f);
    }
}

//: The breath actually moves -- a wave that returned zero forever would pass every bound above.
void the_breath_actually_moves() {
    roboface::IdleLoop loop{5};
    float lowest = 999.0f;
    float highest = -999.0f;
    for (uint32_t elapsed = 0; elapsed < roboface::kBreathPeriodMs * 2; elapsed += 10) {
        const auto offsets = loop.advance(10);
        if (offsets.bob_y < lowest) lowest = offsets.bob_y;
        if (offsets.bob_y > highest) highest = offsets.bob_y;
    }
    TEST_ASSERT_TRUE(highest > roboface::kBreathAmplitudePx * 0.9f);
    TEST_ASSERT_TRUE(lowest < -roboface::kBreathAmplitudePx * 0.9f);
}

//: The gaze wanders rather than tracing one diagonal: its two axes are not in lockstep.
//:
//: Stated as the property rather than as a moment. Two axes moving together would keep a constant
//: ratio between them, so the test is that the ratio *changes* -- which stays true whatever the
//: periods are, where an assertion about one quadrant only holds for the periods it was written
//: against.
void the_gaze_does_not_move_on_a_diagonal() {
    roboface::IdleLoop loop{5};
    float lowest_ratio = 1e9f;
    float highest_ratio = -1e9f;
    for (uint32_t elapsed = 0; elapsed < roboface::kGazePeriodMs * 4; elapsed += 50) {
        const auto offsets = loop.advance(50);
        if (offsets.gaze_x > 1.0f) {  // away from the axis crossing, where the ratio is noise
            const float ratio = offsets.gaze_y / offsets.gaze_x;
            if (ratio < lowest_ratio) lowest_ratio = ratio;
            if (ratio > highest_ratio) highest_ratio = ratio;
        }
    }
    TEST_ASSERT_TRUE(highest_ratio - lowest_ratio > 0.5f);
}

//: `reset` returns to rest without disturbing the settings, for a face resuming idle after doing
//: something else.
void reset_returns_to_rest() {
    roboface::IdleLoop loop{5};
    loop.setIntensity(0.5f);
    blinkStarts(loop, 20000);
    loop.reset();
    TEST_ASSERT_EQUAL_UINT32(0, loop.elapsedMs());
    TEST_ASSERT_FALSE(loop.isBlinking());
    TEST_ASSERT_EQUAL_FLOAT(0.5f, loop.intensity());
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(the_same_seed_gives_the_same_sequence);
    RUN_TEST(different_seeds_give_different_sequences);
    RUN_TEST(intervals_stay_within_the_band);
    RUN_TEST(a_blink_completes_in_its_duration);
    RUN_TEST(intensity_zero_stills_without_stopping);
    RUN_TEST(a_blink_closes_the_eyes_completely);
    RUN_TEST(motion_stays_within_its_amplitudes);
    RUN_TEST(the_breath_actually_moves);
    RUN_TEST(the_gaze_does_not_move_on_a_diagonal);
    RUN_TEST(reset_returns_to_rest);
    return UNITY_END();
}

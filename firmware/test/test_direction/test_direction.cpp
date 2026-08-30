// v2.5 RF-074 — direction from two channels, and the confidence to ignore it.
//
// **The false-positive tests are the ones that matter here**, and they get the most care, for the
// reason v1.4 and v2.4 both learned the hard way: a detector's true-positive case fires while
// someone is holding the device and can see why. Its false positives fire in an empty room, all day,
// and turn a companion into a thing that stares at a fan.

#include <unity.h>

#include <cstdint>
#include <vector>

#include "pure/direction.h"

using roboface::coherence;
using roboface::DirectionEstimate;
using roboface::DirectionEstimator;

namespace {

//: A repeating waveform standing in for a voice: one source, so both channels see the same shape.
std::vector<int16_t> voice(std::size_t count, int16_t amplitude, int period = 37) {
    std::vector<int16_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int phase = static_cast<int>(i % static_cast<std::size_t>(period));
        const int half = period / 2;
        const int ramp = phase < half ? phase : period - phase;
        out[i] = static_cast<int16_t>((2 * amplitude * ramp) / half - amplitude);
    }
    return out;
}

//: Two channels of unrelated sound at the same loudness -- a room, not a person. Two different
//: periods so nothing lines up, and **signed arithmetic throughout**: the `(i % 7) - 3` written
//: against an unsigned index is the trap that produced 47 phantom shakes in v2.4's IMU fixture and
//: cost hours in v1.4 before that.
void room(std::size_t count, int16_t amplitude, std::vector<int16_t>& left,
          std::vector<int16_t>& right) {
    left.resize(count);
    right.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int index = static_cast<int>(i);
        left[i] = static_cast<int16_t>(((index * 13) % 23 - 11) * (amplitude / 11));
        right[i] = static_cast<int16_t>(((index * 7) % 19 - 9) * (amplitude / 9));
    }
}

//: Feed the same frame repeatedly, as the device would every 20 ms, and return the last estimate.
DirectionEstimate settle(DirectionEstimator& estimator, const std::vector<int16_t>& left,
                         const std::vector<int16_t>& right, int frames, uint32_t& clock) {
    DirectionEstimate last;
    for (int i = 0; i < frames; ++i) {
        clock += 20;
        last = estimator.feed(left.data(), right.data(), left.size(), clock);
    }
    return last;
}

constexpr std::size_t kFrame = 320;

}  // namespace

// ---------------------------------------------------------------------------------------
// Coherence — the measure that separates a person from a room
// ---------------------------------------------------------------------------------------

static void test_one_source_is_coherent() {
    const auto signal = voice(kFrame, 9000);
    TEST_ASSERT_TRUE(coherence(signal.data(), signal.data(), kFrame) > 0.95f);
}

static void test_a_room_is_not() {
    std::vector<int16_t> left, right;
    room(kFrame, 9000, left, right);
    const float value = coherence(left.data(), right.data(), kFrame);
    const float magnitude = value < 0.0f ? -value : value;

    TEST_ASSERT_TRUE(magnitude < roboface::kCoherenceThreshold);
}

static void test_coherence_of_nothing_is_zero() {
    std::vector<int16_t> zeros(kFrame, 0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, coherence(zeros.data(), zeros.data(), kFrame));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, coherence(nullptr, zeros.data(), kFrame));
}

// ---------------------------------------------------------------------------------------
// Direction
// ---------------------------------------------------------------------------------------

static void test_a_voice_on_the_left_is_reported_left() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);

    const DirectionEstimate out = settle(estimator, near, far, 20, clock);

    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_TRUE(out.direction < -0.3f);
}

static void test_a_voice_on_the_right_is_reported_right() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);

    const DirectionEstimate out = settle(estimator, far, near, 20, clock);

    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_TRUE(out.direction > 0.3f);
}

static void test_a_centred_voice_has_no_direction_rather_than_a_centred_one() {
    // **The distinction the whole struct exists for.** A speaker in front of the device produces a
    // balance of nearly zero -- which is not evidence of anything, and must not be reported as an
    // instruction to look straight ahead. That would fight the idle drift for control of the gaze.
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto signal = voice(kFrame, 11000);

    const DirectionEstimate out = settle(estimator, signal, signal, 20, clock);

    TEST_ASSERT_FALSE(out.present);
    TEST_ASSERT_TRUE(out.confidence > roboface::kCoherenceThreshold);  // it *was* a person
}

// ---------------------------------------------------------------------------------------
// The false positives — what fires in an empty room
// ---------------------------------------------------------------------------------------

static void test_a_noisy_room_never_becomes_a_direction() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    std::vector<int16_t> left, right;
    room(kFrame, 11000, left, right);

    // Thirty seconds of it -- the length of time a device actually sits on a desk being ignored.
    for (int i = 0; i < 1500; ++i) {
        clock += 20;
        const DirectionEstimate out = estimator.feed(left.data(), right.data(), kFrame, clock);
        TEST_ASSERT_FALSE(out.present);
    }
}

static void test_silence_is_not_a_direction() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    std::vector<int16_t> quiet_l(kFrame), quiet_r(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
        // A near-silent but perfectly correlated pair: the case that passes the coherence test and
        // must still be refused, because two noise floors correlate beautifully.
        const int index = static_cast<int>(i);
        quiet_l[i] = static_cast<int16_t>((index % 5) - 2);
        quiet_r[i] = static_cast<int16_t>(((index % 5) - 2) / 2);
    }

    const DirectionEstimate out = settle(estimator, quiet_l, quiet_r, 30, clock);
    TEST_ASSERT_FALSE(out.present);
}

static void test_identical_channels_report_absent_rather_than_confident_centre() {
    // A board with one microphone, or a second one that died: both channels identical. The estimate
    // must be *absent*, not a confident centre -- the same failure the v2.4 proximity sensor had,
    // where a silent zero was indistinguishable from a measurement.
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto signal = voice(kFrame, 14000);

    const DirectionEstimate out = settle(estimator, signal, signal, 40, clock);
    TEST_ASSERT_FALSE(out.present);
}

// ---------------------------------------------------------------------------------------
// Smoothing
// ---------------------------------------------------------------------------------------

static void test_a_speaker_who_moves_is_followed() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);

    const DirectionEstimate left = settle(estimator, near, far, 20, clock);
    TEST_ASSERT_TRUE(left.direction < -0.3f);

    const DirectionEstimate right = settle(estimator, far, near, 20, clock);
    TEST_ASSERT_TRUE(right.direction > 0.3f);
}

static void test_a_pause_does_not_snap_the_face_back_to_centre() {
    // Quick to move, reluctant to forget. A face that recentred between words would look nervous.
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);
    const DirectionEstimate speaking = settle(estimator, near, far, 20, clock);

    std::vector<int16_t> silence(kFrame, 0);
    const DirectionEstimate pausing = settle(estimator, silence, silence, 5, clock);  // 100 ms

    TEST_ASSERT_TRUE(pausing.present);
    const float kept = pausing.direction / speaking.direction;
    TEST_ASSERT_TRUE(kept > 0.6f);
}

static void test_a_speaker_who_leaves_is_eventually_forgotten() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);
    settle(estimator, near, far, 20, clock);

    std::vector<int16_t> silence(kFrame, 0);
    const DirectionEstimate gone = settle(estimator, silence, silence, 120, clock);  // 2.4 s

    TEST_ASSERT_FALSE(gone.present);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, gone.direction);
}

static void test_reset_drops_the_opinion() {
    DirectionEstimator estimator;
    uint32_t clock = 1000;
    const auto near = voice(kFrame, 12000);
    const auto far = voice(kFrame, 6000);
    settle(estimator, near, far, 20, clock);

    estimator.reset();
    std::vector<int16_t> silence(kFrame, 0);
    clock += 20;
    const DirectionEstimate after = estimator.feed(silence.data(), silence.data(), kFrame, clock);

    TEST_ASSERT_FALSE(after.present);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, after.direction);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_one_source_is_coherent);
    RUN_TEST(test_a_room_is_not);
    RUN_TEST(test_coherence_of_nothing_is_zero);
    RUN_TEST(test_a_voice_on_the_left_is_reported_left);
    RUN_TEST(test_a_voice_on_the_right_is_reported_right);
    RUN_TEST(test_a_centred_voice_has_no_direction_rather_than_a_centred_one);
    RUN_TEST(test_a_noisy_room_never_becomes_a_direction);
    RUN_TEST(test_silence_is_not_a_direction);
    RUN_TEST(test_identical_channels_report_absent_rather_than_confident_centre);
    RUN_TEST(test_a_speaker_who_moves_is_followed);
    RUN_TEST(test_a_pause_does_not_snap_the_face_back_to_centre);
    RUN_TEST(test_a_speaker_who_leaves_is_eventually_forgotten);
    RUN_TEST(test_reset_drops_the_opinion);
    return UNITY_END();
}

// Approach and leave, from readings the sensor would actually produce.
//
// The LTR-553's count rises as something gets nearer and is noisy at every distance, so the two
// tests that matter are the ones about a hand that is *not* doing anything decisive: hovering at
// the boundary, and passing by.

#include <unity.h>

#include <vector>

#include "pure/proximity.h"

using roboface::Presence;
using roboface::ProximityDetector;

namespace {

std::vector<Presence> replay(ProximityDetector& detector,
                             const std::vector<uint16_t>& counts, uint32_t step_ms = 50) {
    std::vector<Presence> out;
    uint32_t t = 1000;
    for (const uint16_t count : counts) {
        const Presence presence = detector.feed(count, t);
        if (presence != Presence::kNone) out.push_back(presence);
        t += step_ms;
    }
    return out;
}

std::vector<uint16_t> held(uint16_t count, int samples) {
    return std::vector<uint16_t>(static_cast<std::size_t>(samples), count);
}

}  // namespace

static void test_nothing_there_reports_nothing() {
    ProximityDetector detector;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(replay(detector, held(20, 200)).size()));
}

static void test_a_hand_arriving_is_an_approach() {
    ProximityDetector detector;
    const auto out = replay(detector, {20, 40, 500, 520, 540, 560});

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Presence::kApproach), static_cast<int>(out[0]));
    TEST_ASSERT_TRUE(detector.isNear());
}

static void test_a_hand_leaving_is_a_leave() {
    ProximityDetector detector;
    // Four samples each way: the settle time is 150 ms and the steps are 50, so three would be
    // exactly at the boundary and a fixture that sits on a threshold tests the threshold rather
    // than the behaviour.
    const auto out = replay(detector, {500, 520, 540, 560, 30, 25, 20, 22});

    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Presence::kApproach), static_cast<int>(out[0]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Presence::kLeave), static_cast<int>(out[1]));
    TEST_ASSERT_FALSE(detector.isNear());
}

static void test_a_hand_held_at_the_boundary_does_not_flicker() {
    // **The reason the two thresholds are different, and the reason for the settle time.**
    //
    // A hand hovering exactly where the reading crosses produces **at most one** event over ten
    // seconds -- and zero is a perfectly good answer: it never decisively arrived. What must not
    // happen is a stream of approach/leave pairs, which is what a single threshold with no settle
    // would give, and the face would flicker between waking and relaxing several times a second.
    //
    // The first version of this test asserted exactly one and was wrong about the requirement
    // rather than about the code.
    ProximityDetector detector;
    std::vector<uint16_t> hovering;
    for (int i = 0; i < 200; ++i) hovering.push_back(i % 2 == 0 ? 310 : 290);

    TEST_ASSERT_TRUE(replay(detector, hovering).size() <= 1);
}

static void test_a_hand_that_arrives_and_stays_produces_exactly_one_approach() {
    // The other side: noise *around* a settled reading must not undo it.
    ProximityDetector detector;
    std::vector<uint16_t> arriving;
    for (int i = 0; i < 200; ++i) arriving.push_back(static_cast<uint16_t>(450 + (i % 7) * 20));

    const auto out = replay(detector, arriving);
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Presence::kApproach), static_cast<int>(out[0]));
}

static void test_a_sleeve_passing_over_is_not_an_approach() {
    // The sensor sees a lot that is not a person reaching for it. A reading has to hold before it
    // means anything.
    ProximityDetector detector;
    const auto out = replay(detector, {20, 600, 20, 20, 20}, /*step_ms=*/50);

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(out.size()));
}

static void test_a_deliberate_reach_is_not_missed() {
    // The other side of that: the settle time must be shorter than a reach, or the face wakes after
    // the hand has already arrived.
    ProximityDetector detector;
    std::vector<uint16_t> reaching;
    for (int i = 0; i < 8; ++i) reaching.push_back(static_cast<uint16_t>(400 + i * 20));

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(replay(detector, reaching, 50).size()));
}

static void test_the_thresholds_have_a_gap() {
    TEST_ASSERT_TRUE(roboface::kFarCount < roboface::kNearCount);
}

static void test_noise_around_a_settled_state_changes_nothing() {
    ProximityDetector detector;
    replay(detector, {500, 520, 540, 560});
    TEST_ASSERT_TRUE(detector.isNear());

    std::vector<uint16_t> wobbling;
    for (int i = 0; i < 200; ++i) wobbling.push_back(static_cast<uint16_t>(480 + (i % 5) * 15));

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(replay(detector, wobbling).size()));
}

static void test_reset_forgets_a_hand() {
    ProximityDetector detector;
    replay(detector, {500, 520, 540, 560});
    detector.reset();
    TEST_ASSERT_FALSE(detector.isNear());
}

static void test_the_gaze_pull_is_a_turn_not_a_stare() {
    // The device looks *toward* a hand; it does not lock onto it.
    TEST_ASSERT_TRUE(roboface::kGazePull > 0.0f);
    TEST_ASSERT_TRUE(roboface::kGazePull < 1.0f);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_there_reports_nothing);
    RUN_TEST(test_a_hand_arriving_is_an_approach);
    RUN_TEST(test_a_hand_leaving_is_a_leave);
    RUN_TEST(test_a_hand_held_at_the_boundary_does_not_flicker);
    RUN_TEST(test_a_hand_that_arrives_and_stays_produces_exactly_one_approach);
    RUN_TEST(test_a_sleeve_passing_over_is_not_an_approach);
    RUN_TEST(test_a_deliberate_reach_is_not_missed);
    RUN_TEST(test_the_thresholds_have_a_gap);
    RUN_TEST(test_noise_around_a_settled_state_changes_nothing);
    RUN_TEST(test_reset_forgets_a_hand);
    RUN_TEST(test_the_gaze_pull_is_a_turn_not_a_stare);
    return UNITY_END();
}

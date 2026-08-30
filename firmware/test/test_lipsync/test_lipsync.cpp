#include <unity.h>

#include "pure/lipsync.h"

namespace {

using roboface::LipSync;
using roboface::MouthFrame;

//: Silence keeps the mouth shut.
void silence_keeps_the_mouth_closed() {
    LipSync lips;
    for (int i = 0; i < 50; ++i) TEST_ASSERT_TRUE(lips.feed(0.0f) == MouthFrame::kClosed);
}

//: A louder moment opens it further. This is the whole feature in one assertion.
void louder_opens_further() {
    LipSync lips;
    TEST_ASSERT_TRUE(lips.feed(0.07f) == MouthFrame::kAjar);
    TEST_ASSERT_TRUE(lips.feed(0.20f) == MouthFrame::kHalf);
    TEST_ASSERT_TRUE(lips.feed(0.50f) == MouthFrame::kOpen);
}

//: And it closes on the way back down, all the way.
void quieter_closes_again() {
    LipSync lips;
    lips.feed(0.9f);
    TEST_ASSERT_TRUE(lips.feed(0.0f) == MouthFrame::kClosed);
}

//: **The reason the hysteresis exists.** A level sitting exactly between two thresholds must not
//: flip on every frame: that flutters visibly, and -- because the face redraws whenever the shape
//: changes -- it costs more than the smooth mouth this replaced.
void a_level_between_thresholds_does_not_flutter() {
    LipSync lips;
    lips.feed(0.20f);
    const auto settled = lips.frame();
    // Wobble around that level the way a real playback envelope does.
    for (int i = 0; i < 40; ++i) {
        TEST_ASSERT_TRUE(lips.feed(i % 2 == 0 ? 0.195f : 0.205f) == settled);
    }
}

//: Opening and closing thresholds are genuinely different for every shape -- the property the test
//: above depends on, stated directly so a future edit to the table cannot quietly remove it.
void every_shape_has_hysteresis() {
    for (std::size_t i = 1; i < static_cast<std::size_t>(MouthFrame::kCount); ++i) {
        TEST_ASSERT_TRUE(roboface::kMouthSteps[i].closes_at < roboface::kMouthSteps[i].opens_at);
    }
}

//: The shapes are ordered: each opens later and travels further than the one below it. A table
//: edited out of order would make the scan in `feed` skip shapes.
void the_table_is_ordered() {
    for (std::size_t i = 2; i < static_cast<std::size_t>(MouthFrame::kCount); ++i) {
        TEST_ASSERT_TRUE(roboface::kMouthSteps[i].opens_at > roboface::kMouthSteps[i - 1].opens_at);
        TEST_ASSERT_TRUE(roboface::kMouthSteps[i].travel > roboface::kMouthSteps[i - 1].travel);
    }
}

//: A jump straight to a loud level reaches the top shape in one step, rather than climbing one
//: shape per frame -- speech starts abruptly and the mouth has to keep up.
void a_sudden_loud_moment_opens_fully_at_once() {
    LipSync lips;
    TEST_ASSERT_TRUE(lips.feed(0.9f) == MouthFrame::kOpen);
}

//: And a sudden silence shuts it at once rather than stepping down.
void a_sudden_silence_closes_at_once() {
    LipSync lips;
    lips.feed(0.9f);
    TEST_ASSERT_TRUE(lips.feed(0.001f) == MouthFrame::kClosed);
}

//: `reset` shuts the mouth when the reply ends.
void reset_shuts_the_mouth() {
    LipSync lips;
    lips.feed(0.9f);
    lips.reset();
    TEST_ASSERT_TRUE(lips.frame() == MouthFrame::kClosed);
}

//: The closed shape adds nothing, so a silent face wears exactly the expression it was given.
void a_closed_mouth_changes_nothing() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, roboface::travelFor(MouthFrame::kClosed));
    TEST_ASSERT_TRUE(roboface::travelFor(MouthFrame::kOpen) > 0.0f);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(silence_keeps_the_mouth_closed);
    RUN_TEST(louder_opens_further);
    RUN_TEST(quieter_closes_again);
    RUN_TEST(a_level_between_thresholds_does_not_flutter);
    RUN_TEST(every_shape_has_hysteresis);
    RUN_TEST(the_table_is_ordered);
    RUN_TEST(a_sudden_loud_moment_opens_fully_at_once);
    RUN_TEST(a_sudden_silence_closes_at_once);
    RUN_TEST(reset_shuts_the_mouth);
    RUN_TEST(a_closed_mouth_changes_nothing);
    return UNITY_END();
}

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
    TEST_ASSERT_TRUE(lips.feed(0.22f) == MouthFrame::kHalf);
    TEST_ASSERT_TRUE(lips.feed(0.50f) == MouthFrame::kWide);
    TEST_ASSERT_TRUE(lips.feed(0.90f) == MouthFrame::kOpen);
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
    lips.feed(0.22f);
    const auto settled = lips.frame();
    // Wobble around that level the way a real playback envelope does.
    for (int i = 0; i < 40; ++i) {
        TEST_ASSERT_TRUE(lips.feed(i % 2 == 0 ? 0.195f : 0.205f) == settled);
    }
}

//: **The width alternates from syllable to syllable.** Two openings of exactly the same height are
//: drawn as two different shapes -- a spread mouth and a pursed one -- which is what makes a handful
//: of shapes read as speech rather than as a jaw hinging.
void consecutive_syllables_take_different_widths() {
    LipSync lips;
    lips.feed(0.9f);
    const roboface::MouthPose first = lips.pose();
    lips.feed(0.0f);   // the gap between syllables
    lips.feed(0.9f);   // and the next one
    const roboface::MouthPose second = lips.pose();

    TEST_ASSERT_EQUAL_FLOAT(first.open, second.open);          // the same height
    TEST_ASSERT_TRUE(first.width != second.width);             // a different shape
}

//: A mouth held open through a long vowel keeps its shape. The alternation is per syllable, not per
//: frame -- flipping the width on every tick would be a flutter, which is what the hysteresis in
//: this same class exists to prevent.
void a_held_vowel_does_not_change_width() {
    LipSync lips;
    lips.feed(0.9f);
    const float width = lips.pose().width;
    for (int i = 0; i < 30; ++i) {
        lips.feed(0.9f);
        TEST_ASSERT_EQUAL_FLOAT(width, lips.pose().width);
    }
}

//: Both variants are real shapes, for every step. A zero or a negative width would draw nothing or
//: draw inverted, and either would be found on the screen rather than here.
void every_width_is_positive() {
    for (std::size_t i = 0; i < static_cast<std::size_t>(MouthFrame::kCount); ++i) {
        TEST_ASSERT_TRUE(roboface::kMouthSteps[i].spread_width > 0.0f);
        TEST_ASSERT_TRUE(roboface::kMouthSteps[i].round_width > 0.0f);
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
    RUN_TEST(consecutive_syllables_take_different_widths);
    RUN_TEST(a_held_vowel_does_not_change_width);
    RUN_TEST(every_width_is_positive);
    RUN_TEST(every_shape_has_hysteresis);
    RUN_TEST(the_table_is_ordered);
    RUN_TEST(a_sudden_loud_moment_opens_fully_at_once);
    RUN_TEST(a_sudden_silence_closes_at_once);
    RUN_TEST(reset_shuts_the_mouth);
    RUN_TEST(a_closed_mouth_changes_nothing);
    return UNITY_END();
}

// The face the server sends: the vocabulary, the recipe table, and the intensity blend.
//
// Everything here is pure -- no M5, no panel, no clock -- which is what lets the questions be
// asked in a millisecond and answered without a board. The totality tests are the ones that
// matter: a missing case in `recipeFor` does not fail anywhere, it draws a blank face on a desk
// with nothing in any log.

#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "pure/face.h"

using roboface::Emotion;
using roboface::FaceRecipe;
using roboface::kDefaultIntensity;
using roboface::kDefaultTtlMs;

// ---------------------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------------------

static void test_the_enum_is_the_documented_seven() {
    // Membership is the contract: the server's `Emotion` has exactly these, and a value on one
    // side that the other has never heard of is a face that cannot be drawn.
    TEST_ASSERT_EQUAL_INT(7, static_cast<int>(Emotion::kCount));
    TEST_ASSERT_EQUAL_STRING("neutral", roboface::toString(Emotion::kNeutral));
    TEST_ASSERT_EQUAL_STRING("calm", roboface::toString(Emotion::kCalm));
    TEST_ASSERT_EQUAL_STRING("joy", roboface::toString(Emotion::kJoy));
    TEST_ASSERT_EQUAL_STRING("thinking", roboface::toString(Emotion::kThinking));
    TEST_ASSERT_EQUAL_STRING("surprised", roboface::toString(Emotion::kSurprised));
    TEST_ASSERT_EQUAL_STRING("sad", roboface::toString(Emotion::kSad));
    TEST_ASSERT_EQUAL_STRING("error", roboface::toString(Emotion::kError));
}

static void test_every_name_round_trips() {
    for (uint8_t index = 0; index < static_cast<uint8_t>(Emotion::kCount); ++index) {
        const auto emotion = static_cast<Emotion>(index);
        TEST_ASSERT_EQUAL_INT(index,
                              static_cast<int>(roboface::emotionFrom(roboface::toString(emotion))));
    }
}

static void test_an_unknown_name_is_neutral_rather_than_a_failure() {
    // The device applies this rule even though the server already did. The two tiers are
    // separately releasable, so neither may assume the other sanitised what it sent -- and the
    // alternative to coercing here is a blank face.
    for (const char* name : {"happy", "HAPPY", "", "neutral ", "joyful", "\xff"}) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Emotion::kNeutral),
                              static_cast<int>(roboface::emotionFrom(name)));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Emotion::kNeutral),
                          static_cast<int>(roboface::emotionFrom(nullptr)));
}

// ---------------------------------------------------------------------------------------
// The recipe table
// ---------------------------------------------------------------------------------------

static void test_the_table_is_total_over_the_enum() {
    // Not "does it return something" -- `recipeFor` cannot fail to. The check is that every value
    // produces a recipe inside its own documented ranges, which a missing `case` falling through
    // to the default would not.
    for (uint8_t index = 0; index < static_cast<uint8_t>(Emotion::kCount); ++index) {
        const FaceRecipe recipe = roboface::recipeFor(static_cast<Emotion>(index));
        TEST_ASSERT_TRUE(recipe.eye_openness >= 0.0f && recipe.eye_openness <= 1.0f);
        TEST_ASSERT_TRUE(recipe.mouth_curve >= -1.0f && recipe.mouth_curve <= 1.0f);
        TEST_ASSERT_TRUE(recipe.brow_angle >= -1.0f && recipe.brow_angle <= 1.0f);
        TEST_ASSERT_TRUE(recipe.tilt >= -1.0f && recipe.tilt <= 1.0f);
        TEST_ASSERT_TRUE(recipe.dim >= 0.0f && recipe.dim <= 1.0f);
    }
}

static void test_no_two_emotions_look_the_same() {
    // Seven faces that a person can tell apart is the whole point of seven faces.
    for (uint8_t a = 0; a < static_cast<uint8_t>(Emotion::kCount); ++a) {
        for (uint8_t b = static_cast<uint8_t>(a + 1); b < static_cast<uint8_t>(Emotion::kCount);
             ++b) {
            const FaceRecipe first = roboface::recipeFor(static_cast<Emotion>(a));
            const FaceRecipe second = roboface::recipeFor(static_cast<Emotion>(b));
            const bool identical =
                first.eye_openness == second.eye_openness &&
                first.mouth_curve == second.mouth_curve && first.brow_angle == second.brow_angle &&
                first.tilt == second.tilt && first.dim == second.dim;
            TEST_ASSERT_FALSE(identical);
        }
    }
}

static void test_joy_and_sadness_curve_the_mouth_opposite_ways() {
    TEST_ASSERT_TRUE(roboface::recipeFor(Emotion::kJoy).mouth_curve > 0.0f);
    TEST_ASSERT_TRUE(roboface::recipeFor(Emotion::kSad).mouth_curve < 0.0f);
}

static void test_thinking_narrows_and_surprise_widens() {
    // The pair most easily confused if only the mouth carried the difference. What separates them
    // across a room is the eyes and the brows, in opposite directions.
    const FaceRecipe thinking = roboface::recipeFor(Emotion::kThinking);
    const FaceRecipe surprised = roboface::recipeFor(Emotion::kSurprised);
    TEST_ASSERT_TRUE(thinking.eye_openness < surprised.eye_openness);
    TEST_ASSERT_TRUE(thinking.brow_angle > surprised.brow_angle);
}

static void test_only_sadness_is_dimmed() {
    // `error` is deliberately not: a fault is the one thing that must not recede. One is the
    // device being unhappy, the other is something being wrong.
    TEST_ASSERT_TRUE(roboface::recipeFor(Emotion::kSad).dim > 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, roboface::recipeFor(Emotion::kError).dim);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, roboface::recipeFor(Emotion::kJoy).dim);
}

// ---------------------------------------------------------------------------------------
// Intensity
// ---------------------------------------------------------------------------------------

static void test_full_intensity_is_the_recipe_itself() {
    for (uint8_t index = 0; index < static_cast<uint8_t>(Emotion::kCount); ++index) {
        const FaceRecipe recipe = roboface::recipeFor(static_cast<Emotion>(index));
        const FaceRecipe scaled = roboface::withIntensity(recipe, 1.0f);
        TEST_ASSERT_EQUAL_FLOAT(recipe.eye_openness, scaled.eye_openness);
        TEST_ASSERT_EQUAL_FLOAT(recipe.mouth_curve, scaled.mouth_curve);
        TEST_ASSERT_EQUAL_FLOAT(recipe.dim, scaled.dim);
    }
}

static void test_zero_intensity_is_the_resting_face() {
    // **Not a blank one.** Multiplying every field by the intensity would drive `eye_openness` to
    // zero, so a barely-sad face would be a face with its eyes shut -- a different expression, and
    // a far stronger one. "Less of this emotion" means closer to neutral.
    const FaceRecipe neutral = roboface::recipeFor(Emotion::kNeutral);
    const FaceRecipe faint = roboface::withIntensity(roboface::recipeFor(Emotion::kSad), 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(neutral.eye_openness, faint.eye_openness);
    TEST_ASSERT_EQUAL_FLOAT(neutral.mouth_curve, faint.mouth_curve);
    TEST_ASSERT_EQUAL_FLOAT(neutral.dim, faint.dim);
}

static void test_intensity_moves_monotonically_toward_the_emotion() {
    const FaceRecipe joy = roboface::recipeFor(Emotion::kJoy);
    float previous = roboface::withIntensity(joy, 0.0f).mouth_curve;
    for (int step = 1; step <= 10; ++step) {
        const float curve = roboface::withIntensity(joy, static_cast<float>(step) / 10.0f).mouth_curve;
        TEST_ASSERT_TRUE(curve >= previous);
        previous = curve;
    }
    TEST_ASSERT_EQUAL_FLOAT(joy.mouth_curve, previous);
}

static void test_an_out_of_range_intensity_is_clamped_not_inverted() {
    // A frame mid-crossfade or a server that skipped its own clamp must not produce a face that
    // is *more* than the emotion, which is where the recipes stop being calibrated.
    const FaceRecipe joy = roboface::recipeFor(Emotion::kJoy);
    TEST_ASSERT_EQUAL_FLOAT(joy.mouth_curve, roboface::withIntensity(joy, 4.0f).mouth_curve);
    TEST_ASSERT_EQUAL_FLOAT(roboface::recipeFor(Emotion::kNeutral).mouth_curve,
                            roboface::withIntensity(joy, -2.0f).mouth_curve);
}

// ---------------------------------------------------------------------------------------
// The frame's own defaults
// ---------------------------------------------------------------------------------------

static void test_the_frame_defaults_match_the_servers() {
    // Both halves apply these, which is what lets the server omit every optional field still at
    // its default. A disagreement here is a face that differs from the one that was sent, and
    // nothing anywhere would report it.
    const roboface::EmotionFrame frame;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Emotion::kNeutral), static_cast<int>(frame.emotion));
    TEST_ASSERT_EQUAL_FLOAT(kDefaultIntensity, frame.intensity);
    TEST_ASSERT_FALSE(frame.has_gaze);
    TEST_ASSERT_FALSE(frame.speaking);
    TEST_ASSERT_EQUAL_UINT32(kDefaultTtlMs, frame.ttl_ms);
    TEST_ASSERT_EQUAL_UINT32(8000, kDefaultTtlMs);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, kDefaultIntensity);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_enum_is_the_documented_seven);
    RUN_TEST(test_every_name_round_trips);
    RUN_TEST(test_an_unknown_name_is_neutral_rather_than_a_failure);
    RUN_TEST(test_the_table_is_total_over_the_enum);
    RUN_TEST(test_no_two_emotions_look_the_same);
    RUN_TEST(test_joy_and_sadness_curve_the_mouth_opposite_ways);
    RUN_TEST(test_thinking_narrows_and_surprise_widens);
    RUN_TEST(test_only_sadness_is_dimmed);
    RUN_TEST(test_full_intensity_is_the_recipe_itself);
    RUN_TEST(test_zero_intensity_is_the_resting_face);
    RUN_TEST(test_intensity_moves_monotonically_toward_the_emotion);
    RUN_TEST(test_an_out_of_range_intensity_is_clamped_not_inverted);
    RUN_TEST(test_the_frame_defaults_match_the_servers);
    return UNITY_END();
}

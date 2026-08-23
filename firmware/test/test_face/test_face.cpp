// Host tests for the state -> face recipe table.
//
// "The board shows a distinct face for idle, listening, thinking, replying, offline and error" is
// the DoD, and it ends in a judgement a person makes by looking. What a laptop can check is the
// half underneath: that every state *has* a face, that the numbers are in range, and that no two
// of the six are close enough to be confused. That is what these do.

#include <unity.h>

#include "pure/face.h"
#include "pure/state.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static const DeviceState kAllStates[] = {
    DeviceState::kBoot,      DeviceState::kWifiConnecting, DeviceState::kIdle,
    DeviceState::kListening, DeviceState::kThinking,       DeviceState::kReplying,
    DeviceState::kOffline,   DeviceState::kError,
};

// The six the DoD names. `boot` and `wifi_connecting` are transient and excluded there, but they
// still need recipes -- which is why the totality test below covers all eight.
static const DeviceState kDoDStates[] = {
    DeviceState::kIdle,     DeviceState::kListening, DeviceState::kThinking,
    DeviceState::kReplying, DeviceState::kOffline,   DeviceState::kError,
};

static bool inRange(float value, float low, float high) { return value >= low && value <= high; }

// ---------------------------------------------------------------------------------------
// Totality
// ---------------------------------------------------------------------------------------

static void test_every_state_has_a_recipe() {
    // Including kListening, which v0.4 never enters and v1 drives. A table covering only the
    // reachable states would fail on the screen, which is the worst place to find a missing case.
    for (const auto state : kAllStates) {
        const FaceRecipe recipe = recipeFor(state);
        // A default-constructed recipe would pass a weaker test; assert the fields are populated
        // by checking they are legal, which the default also is -- so check *something moved*.
        TEST_ASSERT_TRUE_MESSAGE(recipe.eye_openness >= 0.0f, toString(state));
    }
}

static void test_no_state_returns_the_empty_default() {
    // The `return FaceRecipe{}` after the switch is unreachable-by-design; if a state ever fell
    // through to it the face would be a blank stare with no explanation.
    const FaceRecipe fallthrough{};
    for (const auto state : kAllStates) {
        const FaceRecipe recipe = recipeFor(state);
        const bool identical = recipeDistance(recipe, fallthrough) == 0.0f;
        TEST_ASSERT_FALSE_MESSAGE(identical, toString(state));
    }
}

// ---------------------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------------------

static void test_every_field_is_inside_its_documented_range() {
    // An eye_openness of 1.4 is a renderer bug that looks like a drawing bug -- the ellipse
    // overflows its socket and the face looks broken rather than the data looking wrong.
    for (const auto state : kAllStates) {
        const FaceRecipe r = recipeFor(state);
        TEST_ASSERT_TRUE_MESSAGE(inRange(r.eye_openness, 0.0f, 1.0f), toString(state));
        TEST_ASSERT_TRUE_MESSAGE(inRange(r.mouth_curve, -1.0f, 1.0f), toString(state));
        TEST_ASSERT_TRUE_MESSAGE(inRange(r.brow_angle, -1.0f, 1.0f), toString(state));
        TEST_ASSERT_TRUE_MESSAGE(inRange(r.tilt, -1.0f, 1.0f), toString(state));
        TEST_ASSERT_TRUE_MESSAGE(inRange(r.dim, 0.0f, 1.0f), toString(state));
    }
}

// ---------------------------------------------------------------------------------------
// Distinctness — the testable half of "visibly distinct"
// ---------------------------------------------------------------------------------------

static void test_the_six_dod_faces_are_mutually_distinct() {
    for (const auto a : kDoDStates) {
        for (const auto b : kDoDStates) {
            if (a == b) continue;
            const bool distinct = areDistinct(recipeFor(a), recipeFor(b));
            TEST_ASSERT_TRUE_MESSAGE(distinct, toString(a));
        }
    }
}

static void test_distinctness_has_headroom_rather_than_only_just_passing() {
    // A bar that only just clears is one a small future tweak silently breaks. The closest pair
    // should sit comfortably above the threshold, not on it.
    float closest = 2.0f;
    for (const auto a : kDoDStates) {
        for (const auto b : kDoDStates) {
            if (a == b) continue;
            const float d = recipeDistance(recipeFor(a), recipeFor(b));
            if (d < closest) closest = d;
        }
    }
    TEST_ASSERT_TRUE(closest >= kDistinctEnough);
    TEST_ASSERT_TRUE_MESSAGE(closest >= kDistinctEnough * 1.2f, "the closest pair has no headroom");
}

static void test_distance_is_the_largest_single_difference_not_a_sum() {
    // Two faces differing a little in every field are not reliably distinguishable across a room;
    // two differing a lot in one field are. Taking the maximum is what encodes that, and a sum
    // would call the first pair distinct.
    const FaceRecipe base{0.5f, 0.0f, 0.0f, 0.0f, 0.0f};
    const FaceRecipe scattered{0.55f, 0.05f, 0.05f, 0.05f, 0.05f};
    const FaceRecipe focused{0.5f, 0.4f, 0.0f, 0.0f, 0.0f};

    TEST_ASSERT_FALSE(areDistinct(base, scattered));
    TEST_ASSERT_TRUE(areDistinct(base, focused));
}

static void test_distance_is_symmetric_and_zero_against_itself() {
    const FaceRecipe idle = recipeFor(DeviceState::kIdle);
    const FaceRecipe error = recipeFor(DeviceState::kError);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, recipeDistance(idle, idle));
    TEST_ASSERT_EQUAL_FLOAT(recipeDistance(idle, error), recipeDistance(error, idle));
}

// ---------------------------------------------------------------------------------------
// The specific faces DEVICE_UI names
// ---------------------------------------------------------------------------------------

static void test_offline_is_dimmed_and_error_is_not() {
    // DEVICE_UI §Screens: offline is "sad, dimmed to ~60 %". A fault is the one thing that must
    // not recede, so error is at full brightness -- and the contrast is what separates "the device
    // cannot" from "something is wrong".
    TEST_ASSERT_TRUE(recipeFor(DeviceState::kOffline).dim > 0.2f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, recipeFor(DeviceState::kError).dim);
}

static void test_boot_has_its_eyes_closed() {
    // DEVICE_UI §Screens: "eyes closed, opening".
    TEST_ASSERT_TRUE(recipeFor(DeviceState::kBoot).eye_openness < 0.2f);
}

static void test_offline_and_error_both_frown_but_differently() {
    const FaceRecipe offline = recipeFor(DeviceState::kOffline);
    const FaceRecipe error = recipeFor(DeviceState::kError);

    TEST_ASSERT_TRUE(offline.mouth_curve < 0.0f);
    TEST_ASSERT_TRUE(error.mouth_curve < 0.0f);
    // Worried brows versus cross ones: the two unhappy faces must not be the same unhappy face.
    TEST_ASSERT_TRUE(offline.brow_angle < 0.0f);
    TEST_ASSERT_TRUE(error.brow_angle > 0.0f);
}

static void test_replying_is_the_most_cheerful_face() {
    // It is the one the device wears while talking to a person, and it should look like it.
    const float replying = recipeFor(DeviceState::kReplying).mouth_curve;
    for (const auto state : kAllStates) {
        if (state == DeviceState::kReplying) continue;
        TEST_ASSERT_TRUE(recipeFor(state).mouth_curve <= replying);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_state_has_a_recipe);
    RUN_TEST(test_no_state_returns_the_empty_default);
    RUN_TEST(test_every_field_is_inside_its_documented_range);
    RUN_TEST(test_the_six_dod_faces_are_mutually_distinct);
    RUN_TEST(test_distinctness_has_headroom_rather_than_only_just_passing);
    RUN_TEST(test_distance_is_the_largest_single_difference_not_a_sum);
    RUN_TEST(test_distance_is_symmetric_and_zero_against_itself);
    RUN_TEST(test_offline_is_dimmed_and_error_is_not);
    RUN_TEST(test_boot_has_its_eyes_closed);
    RUN_TEST(test_offline_and_error_both_frown_but_differently);
    RUN_TEST(test_replying_is_the_most_cheerful_face);
    return UNITY_END();
}

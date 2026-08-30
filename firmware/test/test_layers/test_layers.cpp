#include <unity.h>

#include "pure/face.h"
#include "pure/layers.h"

namespace {

using roboface::DeviceState;
using roboface::FaceRecipe;
using roboface::layout;

//: Every state's face lays out. Totality here matters for the same reason it does in the recipe
//: table: a missing case fails on the screen, which is the worst place to discover one.
void every_state_lays_out() {
    const DeviceState states[] = {
        DeviceState::kBoot,      DeviceState::kWifiConnecting, DeviceState::kIdle,
        DeviceState::kListening, DeviceState::kThinking,       DeviceState::kReplying,
        DeviceState::kOffline,   DeviceState::kError,
    };
    for (const auto state : states) {
        const auto bank = layout(roboface::recipeFor(state));
        TEST_ASSERT_TRUE(bank.base.half_width > 0);
        TEST_ASSERT_TRUE(bank.left_eye.half_height >= 0);
    }
}

//: Eye height follows openness, and does so monotonically -- a face that opened its eyes wider at
//: 0.6 than at 0.8 would be very hard to explain from the drawing code.
void eye_height_tracks_openness() {
    int previous = -1;
    for (int step = 0; step <= 10; ++step) {
        FaceRecipe recipe;
        recipe.eye_openness = static_cast<float>(step) / 10.0f;
        const int height = layout(recipe).left_eye.half_height;
        TEST_ASSERT_TRUE(height >= previous);
        previous = height;
    }
    FaceRecipe closed;
    closed.eye_openness = 0.0f;
    TEST_ASSERT_EQUAL_INT(0, layout(closed).left_eye.half_height);
}

//: A smile and a frown are mirror images about the mouth line. This is the test that catches the
//: sign error, and a sign error here means a face that is cheerful when it should be sad -- which
//: no assertion about magnitudes alone would notice.
void a_smile_and_a_frown_mirror_each_other() {
    FaceRecipe smile;
    smile.mouth_curve = 0.6f;
    FaceRecipe frown;
    frown.mouth_curve = -0.6f;

    const auto smiling = layout(smile).mouth;
    const auto frowning = layout(frown).mouth;

    // Corners are shared; only the middle moves, and it moves the opposite way by the same amount.
    TEST_ASSERT_EQUAL_INT(smiling.left_y, frowning.left_y);
    TEST_ASSERT_EQUAL_INT(smiling.right_y, frowning.right_y);
    TEST_ASSERT_EQUAL_INT(smiling.left_y - smiling.mid_y, frowning.mid_y - frowning.left_y);
    // And a smile's middle sits *above* the corners on a screen whose y grows downward.
    TEST_ASSERT_TRUE(smiling.mid_y < smiling.left_y);
    TEST_ASSERT_TRUE(frowning.mid_y > frowning.left_y);
}

//: `dim` is a property of the face, not of a layer: one brightness for the whole bank, so a tired
//: face cannot come apart into differently-lit pieces.
void dim_scales_the_whole_bank() {
    FaceRecipe bright;
    bright.dim = 0.0f;
    TEST_ASSERT_EQUAL_UINT8(255, layout(bright).brightness);

    FaceRecipe dark;
    dark.dim = 1.0f;
    TEST_ASSERT_EQUAL_UINT8(0, layout(dark).brightness);

    FaceRecipe half;
    half.dim = 0.5f;
    const uint8_t mid = layout(half).brightness;
    TEST_ASSERT_TRUE(mid > 120 && mid < 136);
}

//: Tilt shifts the features and leaves the glow where it is -- the wash is the room behind the
//: head, and a background that slid with the head would read as the whole world leaning.
void tilt_moves_the_features_not_the_glow() {
    FaceRecipe left;
    left.tilt = -1.0f;
    FaceRecipe right;
    right.tilt = 1.0f;

    const auto leaning_left = layout(left);
    const auto leaning_right = layout(right);

    TEST_ASSERT_TRUE(leaning_left.base.centre_x < leaning_right.base.centre_x);
    TEST_ASSERT_EQUAL_INT(leaning_left.glow.centre_x, leaning_right.glow.centre_x);
}

//: Brows tilt rather than slide: the inner and outer ends move in opposite directions.
void brows_tilt_rather_than_slide() {
    FaceRecipe cross;
    cross.brow_angle = 1.0f;
    const auto brow = layout(cross).left_brow;
    // Inner end down, outer end up, on a screen whose y grows downward.
    TEST_ASSERT_TRUE(brow.inner_y > brow.outer_y);

    FaceRecipe worried;
    worried.brow_angle = -1.0f;
    const auto worried_brow = layout(worried).left_brow;
    TEST_ASSERT_TRUE(worried_brow.inner_y < worried_brow.outer_y);
}

//: A recipe interpolated mid-crossfade can briefly sit outside 0..1. Clamping is the guarantee that
//: one such frame does not invert the face, which is extremely visible for something so brief.
void out_of_range_recipes_are_clamped_not_inverted() {
    FaceRecipe wild;
    wild.eye_openness = 1.8f;
    wild.mouth_curve = -3.0f;
    wild.dim = 2.0f;

    const auto bank = layout(wild);
    TEST_ASSERT_TRUE(bank.left_eye.half_height <= roboface::FaceGeometry{}.eye_open_height);
    TEST_ASSERT_EQUAL_UINT8(0, bank.brightness);
    TEST_ASSERT_TRUE(bank.mouth.mid_y > bank.mouth.left_y);  // still a frown, not an inversion
}

//: The layers composite in the documented order, and the enum is what says so.
void the_layer_order_is_the_documented_one() {
    TEST_ASSERT_TRUE(static_cast<int>(roboface::Layer::kGlow) <
                     static_cast<int>(roboface::Layer::kBase));
    TEST_ASSERT_TRUE(static_cast<int>(roboface::Layer::kBase) <
                     static_cast<int>(roboface::Layer::kEyes));
    TEST_ASSERT_TRUE(static_cast<int>(roboface::Layer::kMouth) <
                     static_cast<int>(roboface::Layer::kOverlay));
    TEST_ASSERT_EQUAL_INT(6, static_cast<int>(roboface::Layer::kCount));
}

}  // namespace

//: Openness and curve are independent. The whole reason `open_height` exists: lip-sync drove the
//: curve once, a face already smiling sat at the clamp, and the mouth moved two pixels. If these two
//: ever share a number again, this fails.
void the_mouth_opens_without_changing_its_curve() {
    const roboface::FaceRecipe smiling{0.9f, 0.70f, 0.0f, 0.0f, 0.0f};
    const auto shut = roboface::layout(smiling, {}, 0.0f);
    const auto wide = roboface::layout(smiling, {}, 1.0f);

    TEST_ASSERT_EQUAL_INT(0, shut.mouth.open_height);
    TEST_ASSERT_EQUAL_INT(roboface::FaceGeometry{}.mouth_open_travel, wide.mouth.open_height);
    // The curve is untouched: same corners, same middle.
    TEST_ASSERT_EQUAL_INT(shut.mouth.mid_y, wide.mouth.mid_y);
    TEST_ASSERT_EQUAL_INT(shut.mouth.left_y, wide.mouth.left_y);
}

//: A mid-crossfade or a stray level cannot turn the mouth inside out.
void mouth_openness_is_clamped() {
    const roboface::FaceRecipe neutral{};
    TEST_ASSERT_EQUAL_INT(0, roboface::layout(neutral, {}, -3.0f).mouth.open_height);
    TEST_ASSERT_EQUAL_INT(roboface::FaceGeometry{}.mouth_open_travel,
                          roboface::layout(neutral, {}, 9.0f).mouth.open_height);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(every_state_lays_out);
    RUN_TEST(eye_height_tracks_openness);
    RUN_TEST(a_smile_and_a_frown_mirror_each_other);
    RUN_TEST(dim_scales_the_whole_bank);
    RUN_TEST(tilt_moves_the_features_not_the_glow);
    RUN_TEST(brows_tilt_rather_than_slide);
    RUN_TEST(out_of_range_recipes_are_clamped_not_inverted);
    RUN_TEST(the_layer_order_is_the_documented_one);
    RUN_TEST(the_mouth_opens_without_changing_its_curve);
    RUN_TEST(mouth_openness_is_clamped);
    return UNITY_END();
}

// The reflex layer: an overlay with a deadline.
//
// Two properties carry this file, and both are about what a reflex must *not* do. It must not
// outlive itself -- v2.3's review named "a signal that stops moving" as this subsystem's recurring
// failure, and a reflex is the only animation whose normal state is "not running". And it must not
// touch the mouth while the device speaks, because the lip-sync owns it twenty times a second.

#include <unity.h>

#include "pure/reflex.h"

using roboface::FaceRecipe;
using roboface::Reflex;
using roboface::ReflexLayer;

namespace {

//: The face a reflex is applied over -- an ordinary neutral expression.
FaceRecipe base() { return roboface::recipeFor(roboface::Emotion::kNeutral); }

bool same(const FaceRecipe& a, const FaceRecipe& b) {
    return a.eye_openness == b.eye_openness && a.mouth_curve == b.mouth_curve &&
           a.brow_angle == b.brow_angle && a.tilt == b.tilt && a.dim == b.dim;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// It stops
// ---------------------------------------------------------------------------------------

static void test_nothing_running_changes_nothing() {
    // The normal state. A reflex is punctuation, not a sentence.
    ReflexLayer layer;
    TEST_ASSERT_TRUE(same(base(), layer.apply(base(), 5000)));
    TEST_ASSERT_FALSE(layer.isActive(5000));
}

static void test_a_reflex_expires_on_its_own() {
    // **The property this subsystem keeps failing.** A reflex that did not stop would leave the
    // face wearing whatever a poke did to it until the next `emotion{}` -- which on an idle device
    // is never.
    ReflexLayer layer;
    layer.fire(Reflex::kStartle, 1000);

    TEST_ASSERT_TRUE(layer.isActive(1100));
    TEST_ASSERT_FALSE(layer.isActive(1000 + roboface::reflexDurationMs(Reflex::kStartle)));
}

static void test_an_expired_reflex_leaves_the_expression_untouched() {
    ReflexLayer layer;
    layer.fire(Reflex::kDizzy, 1000);
    const uint32_t after = 1000 + roboface::reflexDurationMs(Reflex::kDizzy) + 1;

    TEST_ASSERT_TRUE(same(base(), layer.apply(base(), after)));
}

static void test_every_reflex_has_a_duration_and_none_is_long() {
    // None approaches the ~3 s DEVICE_UI gives chrome to fade: a reflex that lasted that long would
    // be an expression, and expressions come from the server.
    for (uint8_t i = 1; i < static_cast<uint8_t>(Reflex::kCount); ++i) {
        const uint32_t duration = roboface::reflexDurationMs(static_cast<Reflex>(i));
        TEST_ASSERT_TRUE(duration > 0);
        TEST_ASSERT_TRUE(duration < 2000);
    }
}

static void test_a_new_reflex_replaces_the_running_one() {
    // Two at once would be two things modulating one face, and the second would look like the first
    // misbehaving.
    ReflexLayer layer;
    layer.fire(Reflex::kTickle, 1000);
    layer.fire(Reflex::kStartle, 1100);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reflex::kStartle), static_cast<int>(layer.current()));
}

// ---------------------------------------------------------------------------------------
// It does not take the mouth while the device is speaking
// ---------------------------------------------------------------------------------------

static void test_a_reflex_leaves_the_mouth_alone_while_speaking() {
    // **Roadmap §v2.4 says this twice**, and it is the reason the two levels exist. The mouth
    // belongs to the lip-sync at 20 FPS; a reflex moving it would be two animations fighting.
    ReflexLayer layer;
    layer.fire(Reflex::kTickle, 1000);

    const FaceRecipe speaking = layer.apply(base(), 1200, /*speaking=*/true);
    TEST_ASSERT_EQUAL_FLOAT(base().mouth_curve, speaking.mouth_curve);
}

static void test_a_reflex_is_still_visible_while_speaking() {
    // The other half: a poke during a reply must not be swallowed entirely. The eyes and brows are
    // free, so it shows -- it just does not take the mouth.
    ReflexLayer layer;
    layer.fire(Reflex::kStartle, 1000);

    const FaceRecipe speaking = layer.apply(base(), 1350, /*speaking=*/true);
    TEST_ASSERT_FALSE(same(base(), speaking));
    TEST_ASSERT_TRUE(speaking.eye_openness > base().eye_openness ||
                     speaking.brow_angle < base().brow_angle);
}

static void test_every_reflex_respects_speaking() {
    for (uint8_t i = 1; i < static_cast<uint8_t>(Reflex::kCount); ++i) {
        ReflexLayer layer;
        const Reflex reflex = static_cast<Reflex>(i);
        layer.fire(reflex, 1000);
        const uint32_t mid = 1000 + roboface::reflexDurationMs(reflex) / 2;
        TEST_ASSERT_EQUAL_FLOAT(base().mouth_curve,
                                layer.apply(base(), mid, /*speaking=*/true).mouth_curve);
    }
}

// ---------------------------------------------------------------------------------------
// Shape, and the affection that builds
// ---------------------------------------------------------------------------------------

static void test_a_reflex_swells_and_subsides() {
    // It reads as a movement rather than as a jump, which at these durations is the difference
    // between a reaction and a glitch.
    ReflexLayer layer;
    layer.fire(Reflex::kStartle, 1000);
    const uint32_t span = roboface::reflexDurationMs(Reflex::kStartle);

    const float early = layer.apply(base(), 1000 + span / 10).eye_openness;
    const float peak = layer.apply(base(), 1000 + span / 2).eye_openness;
    const float late = layer.apply(base(), 1000 + span * 9 / 10).eye_openness;

    TEST_ASSERT_TRUE(peak > early);
    TEST_ASSERT_TRUE(peak > late);
}

static void test_repeated_taps_build_joy() {
    // DEVICE_UI: "repeated taps build joy". The count is what builds it.
    ReflexLayer layer;
    layer.tapped(1000);
    const float once = layer.affection(1000);
    layer.tapped(1100);
    layer.tapped(1200);

    TEST_ASSERT_TRUE(layer.affection(1200) > once);
}

static void test_affection_is_capped() {
    // Someone tapping thirty times should not produce a face thirty times happier than one tapping
    // three.
    ReflexLayer layer;
    for (uint32_t t = 1000; t < 4000; t += 100) layer.tapped(t);

    TEST_ASSERT_TRUE(layer.affection(4000) <= roboface::kAffectionCap);
}

static void test_affection_fades() {
    // **The recurring question, asked of this too**: what happens when it stops being updated? Joy
    // that never faded would leave the device permanently delighted by something that happened
    // yesterday.
    ReflexLayer layer;
    layer.tapped(1000);
    TEST_ASSERT_TRUE(layer.affection(1000) > 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, layer.affection(1000 + roboface::kAffectionFadeMs));
}

static void test_a_deeper_tickle_for_more_taps() {
    ReflexLayer once;
    once.tapped(1000);
    once.fire(Reflex::kTickle, 1000);

    ReflexLayer many;
    for (uint32_t t = 900; t <= 1000; t += 25) many.tapped(t);
    many.fire(Reflex::kTickle, 1000);

    const uint32_t mid = 1000 + roboface::reflexDurationMs(Reflex::kTickle) / 2;
    TEST_ASSERT_TRUE(many.apply(base(), mid).mouth_curve >= once.apply(base(), mid).mouth_curve);
}

static void test_a_reflex_never_inverts_the_face() {
    // A reflex is added on top of an unknown base -- including whatever v2.6's skins produce -- so
    // every field is clamped for the reason `layout.h` clamps: a face inverted for one frame is
    // very visible.
    for (uint8_t i = 1; i < static_cast<uint8_t>(Reflex::kCount); ++i) {
        ReflexLayer layer;
        const Reflex reflex = static_cast<Reflex>(i);
        layer.fire(reflex, 1000);
        for (uint8_t e = 0; e < static_cast<uint8_t>(roboface::Emotion::kCount); ++e) {
            const FaceRecipe from = roboface::recipeFor(static_cast<roboface::Emotion>(e));
            for (uint32_t step = 0; step < roboface::reflexDurationMs(reflex); step += 50) {
                const FaceRecipe out = layer.apply(from, 1000 + step);
                TEST_ASSERT_TRUE(out.eye_openness >= 0.0f && out.eye_openness <= 1.0f);
                TEST_ASSERT_TRUE(out.mouth_curve >= -1.0f && out.mouth_curve <= 1.0f);
                TEST_ASSERT_TRUE(out.brow_angle >= -1.0f && out.brow_angle <= 1.0f);
                TEST_ASSERT_TRUE(out.dim >= 0.0f && out.dim <= 1.0f);
            }
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_running_changes_nothing);
    RUN_TEST(test_a_reflex_expires_on_its_own);
    RUN_TEST(test_an_expired_reflex_leaves_the_expression_untouched);
    RUN_TEST(test_every_reflex_has_a_duration_and_none_is_long);
    RUN_TEST(test_a_new_reflex_replaces_the_running_one);
    RUN_TEST(test_a_reflex_leaves_the_mouth_alone_while_speaking);
    RUN_TEST(test_a_reflex_is_still_visible_while_speaking);
    RUN_TEST(test_every_reflex_respects_speaking);
    RUN_TEST(test_a_reflex_swells_and_subsides);
    RUN_TEST(test_repeated_taps_build_joy);
    RUN_TEST(test_affection_is_capped);
    RUN_TEST(test_affection_fades);
    RUN_TEST(test_a_deeper_tickle_for_more_taps);
    RUN_TEST(test_a_reflex_never_inverts_the_face);
    return UNITY_END();
}

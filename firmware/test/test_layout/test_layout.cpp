// Host tests for the screen geometry.
//
// "Never block the face" is a DEVICE_UI rule, and it is a property of these numbers. Checking it
// here means a laptop proves the bands cannot overlap; checking it on the board means a person
// squinting at a 320x240 panel deciding whether a battery pill is touching an eyebrow.

#include <unity.h>

#include "pure/layout.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static void test_the_screen_is_the_documented_size() {
    TEST_ASSERT_EQUAL_INT(320, kScreenWidth);
    TEST_ASSERT_EQUAL_INT(240, kScreenHeight);
}

static void test_the_face_safe_area_is_the_documented_size_and_centred() {
    TEST_ASSERT_EQUAL_INT(264, kFaceWidth);
    TEST_ASSERT_EQUAL_INT(184, kFaceHeight);
    TEST_ASSERT_EQUAL_INT(28, kFaceLeft);
    TEST_ASSERT_EQUAL_INT(28, kFaceTop);
    // Centred: the margins on both sides match.
    TEST_ASSERT_EQUAL_INT(kFaceLeft, kScreenWidth - kFaceRight);
    TEST_ASSERT_EQUAL_INT(kFaceTop, kScreenHeight - kFaceBottom);
}

static void test_the_chrome_band_is_28_px() {
    // DEVICE_UI §Rules: "Chrome lives in the outer 28 px band."
    TEST_ASSERT_EQUAL_INT(28, kBandHeight);
}

static void test_the_top_band_is_clear_of_the_face() {
    TEST_ASSERT_TRUE(clearOfFace(0, 0, kScreenWidth, kBandHeight));
}

static void test_the_bottom_band_is_clear_of_the_face() {
    TEST_ASSERT_TRUE(clearOfFace(0, kFaceBottom, kScreenWidth, kBandHeight));
}

static void test_the_status_cluster_is_clear_of_the_face() {
    // Top-right, 12 px glyphs. The cluster is where a battery pill would collide with an eyebrow
    // if the numbers were a few pixels out.
    const int x = kStatusClusterRight - 3 * kGlyphSize;
    TEST_ASSERT_TRUE(clearOfFace(x, kStatusClusterTop, 3 * kGlyphSize, kGlyphSize));
}

static void test_something_overlapping_the_face_is_reported_as_such() {
    // The negative case: the helper must actually be able to say no, or every assertion above is
    // vacuous.
    TEST_ASSERT_FALSE(clearOfFace(kFaceLeft + 10, kFaceTop + 10, 20, 20));
    TEST_ASSERT_FALSE(clearOfFace(0, 0, kScreenWidth, kScreenHeight));
    // Straddling the boundary counts as overlapping.
    TEST_ASSERT_FALSE(clearOfFace(kFaceLeft - 5, kFaceTop - 5, 20, 20));
}

static void test_the_face_area_reports_itself_as_inside() {
    TEST_ASSERT_TRUE(insideFace(kFaceLeft, kFaceTop, kFaceWidth, kFaceHeight));
    TEST_ASSERT_FALSE(insideFace(kFaceLeft - 1, kFaceTop, kFaceWidth, kFaceHeight));
    TEST_ASSERT_FALSE(insideFace(kFaceLeft, kFaceTop, kFaceWidth + 1, kFaceHeight));
}

static void test_the_bands_and_the_face_tile_the_screen_without_gaps() {
    // Top band + face + bottom band = the whole height. A gap would be a strip nothing owns, which
    // is where stale pixels survive a redraw.
    TEST_ASSERT_EQUAL_INT(kScreenHeight, kBandHeight + kFaceHeight + kBandHeight);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_screen_is_the_documented_size);
    RUN_TEST(test_the_face_safe_area_is_the_documented_size_and_centred);
    RUN_TEST(test_the_chrome_band_is_28_px);
    RUN_TEST(test_the_top_band_is_clear_of_the_face);
    RUN_TEST(test_the_bottom_band_is_clear_of_the_face);
    RUN_TEST(test_the_status_cluster_is_clear_of_the_face);
    RUN_TEST(test_something_overlapping_the_face_is_reported_as_such);
    RUN_TEST(test_the_face_area_reports_itself_as_inside);
    RUN_TEST(test_the_bands_and_the_face_tile_the_screen_without_gaps);
    return UNITY_END();
}

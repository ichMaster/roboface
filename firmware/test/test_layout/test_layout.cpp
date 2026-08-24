// Host tests for the screen geometry.
//
// "Never block the face" is a DEVICE_UI rule, and it is a property of these numbers. Checking it
// here means a laptop proves the bands cannot overlap; checking it on the board means a person
// squinting at a 320x240 panel deciding whether a battery pill is touching an eyebrow.

#include <unity.h>

#include "pure/layout.h"
#include "pure/transcript.h"

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


// --- the console's text grid (v0.5) ---------------------------------------------------
//
// The font is a glue asset, so a laptop cannot check that a glyph looks right. What it can check
// is the arithmetic that decides whether text fits -- and getting that wrong does not look like a
// font problem on the panel, it looks like the last line of every reply being cut off, or text
// bleeding into the chrome band.

void test_the_console_grid_is_the_largest_that_fits_the_face_area() {
    // 264/10 and 184/20 both leave a remainder -- 260x180 used of 264x184 -- so the grid is the
    // largest that fits rather than an exact division. Both halves of that matter: it must not
    // exceed the area (or text bleeds into the chrome band), and it must not be smaller than it
    // could be (or the panel wastes a column and the last line of a long reply is cut).
    TEST_ASSERT_TRUE(kConsoleColumns * kConsoleAdvanceWidth <= kFaceWidth);
    TEST_ASSERT_TRUE((kConsoleColumns + 1) * kConsoleAdvanceWidth > kFaceWidth);
    TEST_ASSERT_TRUE(kConsoleLines * kConsoleLineHeight <= kFaceHeight);
    TEST_ASSERT_TRUE((kConsoleLines + 1) * kConsoleLineHeight > kFaceHeight);
}

void test_the_console_grid_is_the_documented_26_by_9() {
    TEST_ASSERT_EQUAL_INT(26, kConsoleColumns);
    TEST_ASSERT_EQUAL_INT(9, kConsoleLines);
}

void test_a_full_console_stays_inside_the_face_area() {
    // Every column and every line drawn, at the face's origin: still clear of the chrome bands.
    TEST_ASSERT_TRUE(insideFace(kFaceLeft, kFaceTop,
                                kConsoleColumns * kConsoleAdvanceWidth,
                                kConsoleLines * kConsoleLineHeight));
    // And one line more would not be.
    TEST_ASSERT_FALSE(insideFace(kFaceLeft, kFaceTop,
                                 kConsoleColumns * kConsoleAdvanceWidth,
                                 (kConsoleLines + 1) * kConsoleLineHeight));
}

void test_the_transcript_measures_against_the_same_numbers() {
    // The wrap and the renderer must agree on the width, or the text is either clipped at the
    // right edge or wrapped short of it. One constant, both users.
    const roboface::Transcript t;
    TEST_ASSERT_EQUAL_UINT32(static_cast<std::size_t>(kConsoleColumns), t.columns());
    TEST_ASSERT_EQUAL_UINT32(static_cast<std::size_t>(kConsoleLines), t.maxLines());
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
    RUN_TEST(test_the_console_grid_is_the_largest_that_fits_the_face_area);
    RUN_TEST(test_the_console_grid_is_the_documented_26_by_9);
    RUN_TEST(test_a_full_console_stays_inside_the_face_area);
    RUN_TEST(test_the_transcript_measures_against_the_same_numbers);
    return UNITY_END();
}

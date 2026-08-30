// v2.6 RF-078 — a skin is data, and the data is total over the emotions.
//
// The property under test is not "this manifest is right". It is **"no manifest can be partly
// right"** — a skin that answers for six emotions and not the seventh is a face that vanishes when
// someone is sad, and it is exactly the kind of gap that ships, because the missing case is rare by
// construction and so nobody exercises it.

#include <unity.h>

#include "pure/skin.h"

using roboface::Emotion;
using roboface::Skin;
using roboface::SkinBody;
using roboface::SkinElement;
using roboface::SkinFault;
using roboface::validate;

namespace {

//: A manifest that is correct, so each test can spoil exactly one thing about it. Building the
//: broken cases from a good one is what keeps them honest: a test that constructs its own invalid
//: skin from scratch can be passing because of a second mistake nobody noticed.
Skin sound() {
    Skin skin;
    skin.name = "test";
    skin.ink = 0x5FFF;
    skin.highlight = 0xFFFF;
    skin.body_colour = 0xFFFF;
    return skin;
}

Skin withFullPalette(SkinElement element) {
    Skin skin = sound();
    skin.element = element;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Emotion::kCount); ++i) {
        skin.element_palette.colour[i] = static_cast<uint16_t>(0x1000 + i);
    }
    return skin;
}

}  // namespace

static void test_a_sound_manifest_validates() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone), static_cast<int>(validate(sound())));
}

static void test_a_palette_answers_for_every_emotion() {
    // **The totality property, iterated over the enum rather than spelled per emotion.** Written the
    // other way, adding an eighth emotion would leave this test passing and every skin undrawn for
    // the new one.
    const Skin skin = withFullPalette(SkinElement::kPaletteFollowsEmotion);
    for (std::size_t i = 0; i < static_cast<std::size_t>(Emotion::kCount); ++i) {
        const auto emotion = static_cast<Emotion>(i);
        TEST_ASSERT_NOT_EQUAL(0, skin.element_palette.at(emotion));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone), static_cast<int>(validate(skin)));
}

static void test_an_element_missing_one_emotion_is_refused() {
    // One hole, in the middle. A skin whose flame is black when it is thinking.
    Skin skin = withFullPalette(SkinElement::kGlowFollowsEmotion);
    skin.element_palette.colour[static_cast<std::size_t>(Emotion::kThinking)] = 0;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kElementPaletteMissing),
                          static_cast<int>(validate(skin)));
}

static void test_an_element_that_does_not_follow_the_emotion_needs_no_palette() {
    // The ghost's blush is not a per-emotion colour, and requiring one would be the schema
    // inventing work for a skin that does not do it.
    Skin skin = sound();
    skin.element = SkinElement::kBlushAndTear;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone), static_cast<int>(validate(skin)));
}

static void test_a_nameless_skin_is_refused() {
    Skin skin = sound();
    skin.name = "";
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNoName), static_cast<int>(validate(skin)));
    skin.name = nullptr;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNoName), static_cast<int>(validate(skin)));
}

static void test_eyes_may_not_leave_the_face_area() {
    // **The check that pays for this whole function.** A skin whose eyes are 20 px too high does not
    // crash -- it draws over the battery indicator, and someone notices three weeks later.
    Skin skin = sound();
    skin.geometry.eye_offset_y = -120;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kEyesOutsideFace),
                          static_cast<int>(validate(skin)));

    Skin wide = sound();
    wide.geometry.eye_spacing = 400;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kEyesOutsideFace),
                          static_cast<int>(validate(wide)));
}

static void test_the_gaze_travel_counts_against_the_safe_area() {
    // A face that fits at rest and slides over the chrome when it looks sideways. The gaze offset is
    // part of the geometry, so the bounds check has to include it -- otherwise the manifest is valid
    // and the face is wrong only while someone is talking from one side.
    Skin skin = sound();
    skin.geometry.eye_spacing = 180;
    skin.geometry.eye_half_width = 26;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone), static_cast<int>(validate(skin)));

    skin.gaze_travel_px = 40;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kEyesOutsideFace),
                          static_cast<int>(validate(skin)));
}

static void test_the_mouth_may_not_leave_the_face_area_either() {
    Skin skin = sound();
    skin.geometry.mouth_offset_y = 120;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kMouthOutsideFace),
                          static_cast<int>(validate(skin)));
}

static void test_eyes_may_not_overlap() {
    Skin skin = sound();
    skin.geometry.eye_spacing = 10;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kEyesOverlap),
                          static_cast<int>(validate(skin)));
}

static void test_every_fault_has_something_to_say() {
    // A reason, not a bool: RF-083's fallback prints this, and "the file was truncated" and "the
    // eyes are off the screen" must not be the same line in a log.
    for (std::size_t i = 0; i < static_cast<std::size_t>(SkinFault::kCount); ++i) {
        const char* text = roboface::toString(static_cast<SkinFault>(i));
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(text[0] != '\0');
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(SkinElement::kCount); ++i) {
        TEST_ASSERT_NOT_NULL(roboface::toString(static_cast<SkinElement>(i)));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_sound_manifest_validates);
    RUN_TEST(test_a_palette_answers_for_every_emotion);
    RUN_TEST(test_an_element_missing_one_emotion_is_refused);
    RUN_TEST(test_an_element_that_does_not_follow_the_emotion_needs_no_palette);
    RUN_TEST(test_a_nameless_skin_is_refused);
    RUN_TEST(test_eyes_may_not_leave_the_face_area);
    RUN_TEST(test_the_gaze_travel_counts_against_the_safe_area);
    RUN_TEST(test_the_mouth_may_not_leave_the_face_area_either);
    RUN_TEST(test_eyes_may_not_overlap);
    RUN_TEST(test_every_fault_has_something_to_say);
    return UNITY_END();
}

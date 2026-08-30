// v2.6 RF-078 — a skin is data, and the data is total over the emotions.
//
// The property under test is not "this manifest is right". It is **"no manifest can be partly
// right"** — a skin that answers for six emotions and not the seventh is a face that vanishes when
// someone is sad, and it is exactly the kind of gap that ships, because the missing case is rare by
// construction and so nobody exercises it.

#include <unity.h>

#include "pure/skin.h"
#include "pure/skins.h"

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

// ---------------------------------------------------------------------------------------
// RF-079 — the procedural face, expressed as a manifest
// ---------------------------------------------------------------------------------------

static void test_stackchan_is_the_face_it_always_was() {
    // **The regression that would otherwise change the face nobody was looking at.** Moving a face
    // from constants into data is exactly the kind of refactor that arrives with one number
    // transcribed wrong, and the symptom -- eyes 4 px too close together -- is invisible unless you
    // have the old screen beside the new one.
    //
    // So the values are spelled here, not read from the manifest: a test that asked the manifest
    // what it contained would agree with any transcription error it made.
    const roboface::Skin skin = roboface::stackchan();

    TEST_ASSERT_EQUAL_UINT16(0x5DFF, skin.ink);          // the soft cyan-white
    TEST_ASSERT_EQUAL_UINT16(0x0104, skin.body_colour);  // the dark blue wash behind the head
    TEST_ASSERT_EQUAL_UINT16(0x0000, skin.background);   // the panel's own dark is the room
    TEST_ASSERT_EQUAL_INT(14, skin.gaze_travel_px);      // FaceGeometry::max_tilt_px

    // The geometry is `FaceGeometry`'s documented defaults -- unchanged since v2.1.
    TEST_ASSERT_EQUAL_INT(62, skin.geometry.eye_spacing);
    TEST_ASSERT_EQUAL_INT(26, skin.geometry.eye_half_width);
    TEST_ASSERT_EQUAL_INT(30, skin.geometry.eye_open_height);
    TEST_ASSERT_EQUAL_INT(-22, skin.geometry.eye_offset_y);
    TEST_ASSERT_EQUAL_INT(42, skin.geometry.mouth_offset_y);
    TEST_ASSERT_EQUAL_INT(40, skin.geometry.mouth_half_width);
}

static void test_stackchan_validates_like_any_other_skin() {
    // It is not exempt, and being subject to the same check as the spirits is the point of moving
    // it: a schema the known-good face cannot satisfy is a schema that is wrong.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone),
                          static_cast<int>(validate(roboface::stackchan())));
}

static void test_a_uniform_palette_is_still_total() {
    // A skin whose element does not follow the emotion still fills its table. A table that is only
    // sometimes filled in is a table someone will read when it is not.
    const roboface::EmotionPalette palette = roboface::uniform(0x1234);
    for (std::size_t i = 0; i < static_cast<std::size_t>(Emotion::kCount); ++i) {
        TEST_ASSERT_EQUAL_UINT16(0x1234, palette.colour[i]);
    }
}

static void test_the_mood_palette_lands_each_colour_on_its_own_emotion() {
    // Seven positional arguments in enum order: the shape most likely to be silently transposed,
    // and the one whose symptom is a face that is the wrong colour only when it is sad.
    const roboface::EmotionPalette palette =
        roboface::moodPalette(0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007);

    TEST_ASSERT_EQUAL_UINT16(0x0001, palette.at(Emotion::kNeutral));
    TEST_ASSERT_EQUAL_UINT16(0x0002, palette.at(Emotion::kCalm));
    TEST_ASSERT_EQUAL_UINT16(0x0003, palette.at(Emotion::kJoy));
    TEST_ASSERT_EQUAL_UINT16(0x0004, palette.at(Emotion::kThinking));
    TEST_ASSERT_EQUAL_UINT16(0x0005, palette.at(Emotion::kSurprised));
    TEST_ASSERT_EQUAL_UINT16(0x0006, palette.at(Emotion::kSad));
    TEST_ASSERT_EQUAL_UINT16(0x0007, palette.at(Emotion::kError));
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
    RUN_TEST(test_stackchan_is_the_face_it_always_was);
    RUN_TEST(test_stackchan_validates_like_any_other_skin);
    RUN_TEST(test_a_uniform_palette_is_still_total);
    RUN_TEST(test_the_mood_palette_lands_each_colour_on_its_own_emotion);
    return UNITY_END();
}

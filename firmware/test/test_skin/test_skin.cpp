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

// ---------------------------------------------------------------------------------------
// RF-080 — the four spirits
// ---------------------------------------------------------------------------------------

static void test_every_skin_validates() {
    // **Iterated over the whole set, not spelled per skin.** A sixth face added to `skinAt` is
    // covered by this test the moment it exists, which is the difference between a test of the set
    // and five tests that happen to cover it today.
    for (std::size_t i = 0; i < roboface::kSkinCount; ++i) {
        const roboface::Skin skin = roboface::skinAt(i);
        const SkinFault fault = validate(skin);
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(SkinFault::kNone), static_cast<int>(fault),
                                      roboface::toString(fault));
    }
}

static void test_every_skin_answers_for_every_emotion() {
    // The phase DoD, as a property: *"all five faces render the same EmotionFrame correctly"*.
    // A skin whose palette has a hole is a face that vanishes for one mood.
    for (std::size_t i = 0; i < roboface::kSkinCount; ++i) {
        const roboface::Skin skin = roboface::skinAt(i);
        for (std::size_t e = 0; e < static_cast<std::size_t>(Emotion::kCount); ++e) {
            TEST_ASSERT_NOT_EQUAL(0, skin.element_palette.at(static_cast<Emotion>(e)));
        }
    }
}

static void test_every_skin_has_a_distinct_name() {
    // The names are the `face_set` vocabulary. Two skins sharing one would make a switch land on
    // whichever `skinIndexFor` happened to reach first.
    for (std::size_t a = 0; a < roboface::kSkinCount; ++a) {
        for (std::size_t b = a + 1; b < roboface::kSkinCount; ++b) {
            TEST_ASSERT_NOT_EQUAL(roboface::skinIndexFor(roboface::skinAt(a).name),
                                  roboface::skinIndexFor(roboface::skinAt(b).name));
        }
    }
}

static void test_a_name_round_trips_to_its_own_skin() {
    for (std::size_t i = 0; i < roboface::kSkinCount; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i, roboface::skinIndexFor(roboface::skinAt(i).name));
    }
}

static void test_an_unknown_name_is_refused_rather_than_defaulted() {
    // **Not a default of zero.** Silently wearing stackchan would make a typo in a server config
    // indistinguishable from a deliberate choice -- and the server would be told the switch worked.
    TEST_ASSERT_EQUAL_UINT32(roboface::kSkinCount, roboface::skinIndexFor("dragon"));
    TEST_ASSERT_EQUAL_UINT32(roboface::kSkinCount, roboface::skinIndexFor(""));
    TEST_ASSERT_EQUAL_UINT32(roboface::kSkinCount, roboface::skinIndexFor(nullptr));

    // And a prefix is not a match: "gho" is not the ghost.
    TEST_ASSERT_EQUAL_UINT32(roboface::kSkinCount, roboface::skinIndexFor("gho"));
    TEST_ASSERT_EQUAL_UINT32(roboface::kSkinCount, roboface::skinIndexFor("ghostly"));
}

static void test_the_spirits_kept_the_prototypes_anchors() {
    // The coordinates are `face-prototype.html`'s, converted from SVG absolutes to offsets from the
    // face centre (160, 120). Spelled here so a transcription error cannot pass: the prototype says
    // the ghost's eyes are at x = 122 and 198, y = 118.
    const roboface::Skin g = roboface::ghost();
    TEST_ASSERT_EQUAL_INT(76, g.geometry.eye_spacing);   // 198 - 122
    TEST_ASSERT_EQUAL_INT(-2, g.geometry.eye_offset_y);  // 118 - 120
    TEST_ASSERT_EQUAL_INT(38, g.geometry.mouth_offset_y);  // 158 - 120

    const roboface::Skin f = roboface::flame();
    TEST_ASSERT_EQUAL_INT(64, f.geometry.eye_spacing);   // 192 - 128
    TEST_ASSERT_EQUAL_INT(18, f.geometry.eye_offset_y);  // 138 - 120

    const roboface::Skin c = roboface::cloud();
    TEST_ASSERT_EQUAL_INT(60, c.geometry.eye_spacing);   // 190 - 130
    TEST_ASSERT_EQUAL_INT(12, c.geometry.eye_offset_y);  // 132 - 120
}

static void test_each_element_is_used_by_exactly_one_skin() {
    // The enum is closed on purpose, and each entry earns its place. An element nothing uses is an
    // element that will drift; two skins sharing one would mean the schema is describing something
    // other than what makes them different.
    std::size_t seen[static_cast<std::size_t>(SkinElement::kCount)] = {};
    for (std::size_t i = 0; i < roboface::kSkinCount; ++i) {
        ++seen[static_cast<std::size_t>(roboface::skinAt(i).element)];
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(SkinElement::kCount); ++i) {
        TEST_ASSERT_EQUAL_UINT32(1, seen[i]);
    }
}

// ---------------------------------------------------------------------------------------
// RF-083 — a face is never absent
// ---------------------------------------------------------------------------------------

static void test_a_good_manifest_is_worn_rather_than_replaced() {
    // **The mirror-image bug, and the one nobody would notice.** A fallback that fired always would
    // pass every test about broken packs and leave the device permanently procedural -- looking
    // exactly like a device whose four spirits had never been written.
    for (std::size_t i = 0; i < roboface::kSkinCount; ++i) {
        const roboface::SkinLoad loaded = roboface::loadSkinAt(i);
        TEST_ASSERT_FALSE(loaded.fell_back);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone), static_cast<int>(loaded.fault));
        TEST_ASSERT_EQUAL_STRING(roboface::skinAt(i).name, loaded.skin.name);
    }
}

static void test_each_way_a_pack_can_be_broken_falls_back_with_its_own_reason() {
    // A reason per failure, because "the pack was truncated" and "the eyes are off the screen" need
    // different things done about them and would otherwise be the same line in a log.
    struct Case {
        roboface::Skin skin;
        SkinFault expected;
    };

    roboface::Skin nameless = roboface::ghost();
    nameless.name = "";

    roboface::Skin high = roboface::ghost();
    high.geometry.eye_offset_y = -200;

    roboface::Skin low = roboface::ghost();
    low.geometry.mouth_offset_y = 200;

    roboface::Skin crossed = roboface::ghost();
    crossed.geometry.eye_spacing = 4;

    roboface::Skin hole = roboface::flame();
    hole.element_palette.colour[static_cast<std::size_t>(Emotion::kSad)] = 0;

    const Case cases[] = {
        {nameless, SkinFault::kNoName},
        {high, SkinFault::kEyesOutsideFace},
        {low, SkinFault::kMouthOutsideFace},
        {crossed, SkinFault::kEyesOverlap},
        {hole, SkinFault::kElementPaletteMissing},
    };

    for (const auto& item : cases) {
        const roboface::SkinLoad loaded = roboface::loadSkin(item.skin);
        TEST_ASSERT_TRUE(loaded.fell_back);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(item.expected), static_cast<int>(loaded.fault));
        TEST_ASSERT_EQUAL_STRING("stackchan", loaded.skin.name);
    }
}

static void test_an_index_nothing_answers_to_is_a_missing_pack() {
    const roboface::SkinLoad loaded = roboface::loadSkinAt(roboface::kSkinCount + 3);
    TEST_ASSERT_TRUE(loaded.fell_back);
    TEST_ASSERT_EQUAL_STRING("stackchan", loaded.skin.name);
}

static void test_the_fallback_itself_always_validates() {
    // The one that must never fail. If the procedural face could be invalid there would be nothing
    // to fall back *to*, and a device with no face has no way to say so.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SkinFault::kNone),
                          static_cast<int>(validate(roboface::loadSkin(roboface::Skin{}).skin)));
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
    RUN_TEST(test_every_skin_validates);
    RUN_TEST(test_every_skin_answers_for_every_emotion);
    RUN_TEST(test_every_skin_has_a_distinct_name);
    RUN_TEST(test_a_name_round_trips_to_its_own_skin);
    RUN_TEST(test_an_unknown_name_is_refused_rather_than_defaulted);
    RUN_TEST(test_the_spirits_kept_the_prototypes_anchors);
    RUN_TEST(test_each_element_is_used_by_exactly_one_skin);
    RUN_TEST(test_a_good_manifest_is_worn_rather_than_replaced);
    RUN_TEST(test_each_way_a_pack_can_be_broken_falls_back_with_its_own_reason);
    RUN_TEST(test_an_index_nothing_answers_to_is_a_missing_pack);
    RUN_TEST(test_the_fallback_itself_always_validates);
    return UNITY_END();
}

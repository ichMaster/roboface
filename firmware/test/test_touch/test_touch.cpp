// The touch vocabulary, replayed from fixtures.
//
// A gesture is a *sequence*, so every test here feeds one -- samples with coordinates and
// timestamps, the way the panel produces them. The v2.3 lesson applies literally: a fixture that
// does not resemble a finger proves nothing about fingers, so these are built from how a hand
// actually moves rather than from numbers that make the assertions pass.

#include <unity.h>

#include <initializer_list>
#include <vector>

#include "pure/touch.h"

using roboface::TouchGesture;
using roboface::TouchGestures;
using roboface::TouchSample;
using roboface::TouchZone;

namespace {

//: The middle of a cheek, in panel coordinates -- below the eyes, inside the face.
constexpr int kCheekX = 160;
constexpr int kCheekY = 175;
constexpr int kForeheadY = 60;

//: Feed a whole sequence and keep the gestures it produced.
std::vector<roboface::TouchResult> replay(TouchGestures& gestures,
                                          std::initializer_list<TouchSample> samples,
                                          bool speech = false) {
    std::vector<roboface::TouchResult> out;
    for (const auto& sample : samples) {
        const auto result = gestures.feed(sample, speech);
        if (result.gesture != TouchGesture::kNone) out.push_back(result);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Zones follow the face
// ---------------------------------------------------------------------------------------

static void test_outside_the_face_is_not_affection() {
    // The outer 28 px bands are chrome. A touch there belongs to the UI, not to the character.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(5, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(160, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(315, 120)));
}

static void test_an_eye_is_where_the_renderer_draws_one() {
    // **The property that makes this file worth having.** The zone is derived from `layout.h`, so
    // moving the eyes moves the poke zone with them. Writing the coordinates twice would let a
    // change to the face break a poke silently.
    const roboface::FaceGeometry geometry;
    const int eye_y = geometry.centre_y + geometry.eye_offset_y;
    const int left_eye_x = geometry.centre_x - geometry.eye_spacing / 2;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kEye),
                          static_cast<int>(roboface::zoneAt(left_eye_x, eye_y)));
}

static void test_moving_the_eyes_moves_the_poke_zone() {
    roboface::FaceGeometry moved;
    moved.eye_offset_y = -60;  // eyes higher up the face
    const int old_eye_y = roboface::FaceGeometry{}.centre_y + roboface::FaceGeometry{}.eye_offset_y;
    const int new_eye_y = moved.centre_y + moved.eye_offset_y;
    const int x = moved.centre_x - moved.eye_spacing / 2;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kEye),
                          static_cast<int>(roboface::zoneAt(x, new_eye_y, moved)));
    // And the old position is no longer an eye.
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(TouchZone::kEye),
                          static_cast<int>(roboface::zoneAt(x, old_eye_y + 40, moved)));
}

static void test_forehead_and_cheek_split_at_the_eyes() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kForehead),
                          static_cast<int>(roboface::zoneAt(60, kForeheadY)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kCheek),
                          static_cast<int>(roboface::zoneAt(60, kCheekY)));
}

// ---------------------------------------------------------------------------------------
// The gestures DEVICE_UI names
// ---------------------------------------------------------------------------------------

static void test_a_tap_is_a_tap() {
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000},
        {false, kCheekX, kCheekY, 1060},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(out[0].gesture));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kCheek), static_cast<int>(out[0].zone));
    TEST_ASSERT_EQUAL_INT(1, out[0].count);
}

static void test_taps_in_quick_succession_accumulate() {
    // DEVICE_UI: "repeated taps build joy". The count is what builds it.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000}, {false, kCheekX, kCheekY, 1050},
        {true,  kCheekX, kCheekY, 1200}, {false, kCheekX, kCheekY, 1250},
        {true,  kCheekX, kCheekY, 1400}, {false, kCheekX, kCheekY, 1450},
    });

    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(out[0].gesture));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMultiTap), static_cast<int>(out[1].gesture));
    TEST_ASSERT_EQUAL_INT(3, out[2].count);
}

static void test_taps_far_apart_do_not_accumulate() {
    // **The other half of the window's job.** Two deliberate separate touches must not merge, or a
    // touch now inherits the meaning of one from a second ago.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000}, {false, kCheekX, kCheekY, 1050},
        {true,  kCheekX, kCheekY, 3000}, {false, kCheekX, kCheekY, 3050},
    });

    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(1, out[0].count);
    TEST_ASSERT_EQUAL_INT(1, out[1].count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(out[1].gesture));
}

static void test_a_slow_drag_across_the_face_is_a_stroke() {
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  80,  kCheekY, 1000},
        {true,  100, kCheekY, 1100},
        {true,  130, kCheekY, 1200},
        {true,  180, kCheekY, 1300},
        {false, 180, kCheekY, 1400},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kStroke), static_cast<int>(out[0].gesture));
}

static void test_a_stroke_is_reported_while_it_happens() {
    // A contented arc that arrived after the finger left would be a reaction to a memory.
    TouchGestures gestures;
    gestures.feed({true, 80, kCheekY, 1000});
    const auto moving = gestures.feed({true, 180, kCheekY, 1200});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kStroke), static_cast<int>(moving.gesture));
    TEST_ASSERT_TRUE(gestures.isHeld());
}

static void test_a_hold_that_wobbles_is_not_a_stroke() {
    // A finger resting on glass drifts a few pixels. If that were a stroke, every PTT hold would
    // report affection the character never received.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX,     kCheekY,     1000},
        {true,  kCheekX + 4, kCheekY - 3, 1100},
        {true,  kCheekX - 2, kCheekY + 5, 1200},
        {false, kCheekX,     kCheekY,     1300},
    });

    for (const auto& result : out) {
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(TouchGesture::kStroke),
                              static_cast<int>(result.gesture));
    }
}

static void test_a_press_on_an_eye_is_a_poke() {
    const roboface::FaceGeometry geometry;
    const int eye_y = geometry.centre_y + geometry.eye_offset_y;
    const int eye_x = geometry.centre_x + geometry.eye_spacing / 2;

    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  eye_x, eye_y, 1000},
        {false, eye_x, eye_y, 1060},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kPokeEye), static_cast<int>(out[0].gesture));
}

static void test_poking_an_eye_twice_is_still_a_poke() {
    // The reaction DEVICE_UI gives a poke does not accumulate, so the count must not turn it into
    // something else.
    const roboface::FaceGeometry geometry;
    const int eye_y = geometry.centre_y + geometry.eye_offset_y;
    const int eye_x = geometry.centre_x - geometry.eye_spacing / 2;

    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  eye_x, eye_y, 1000}, {false, eye_x, eye_y, 1050},
        {true,  eye_x, eye_y, 1200}, {false, eye_x, eye_y, 1250},
    });

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kPokeEye), static_cast<int>(out[1].gesture));
}

// ---------------------------------------------------------------------------------------
// The hold's two outcomes
// ---------------------------------------------------------------------------------------

static void test_a_hold_past_the_ptt_threshold_is_a_long_press() {
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000},
        {true,  kCheekX, kCheekY, 1300},
        {false, kCheekX, kCheekY, 1400},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kLongPress),
                          static_cast<int>(out[0].gesture));
}

static void test_a_hold_past_1200ms_without_speech_becomes_the_carousels() {
    // v2.6 turns this into the skin carousel. This phase only has to tell it apart.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true, kCheekX, kCheekY, 1000},
        {true, kCheekX, kCheekY, 1900},
        {true, kCheekX, kCheekY, 2300},
        {false, kCheekX, kCheekY, 2400},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kHeldSilent),
                          static_cast<int>(out[0].gesture));
}

static void test_speaking_keeps_a_long_hold_a_ptt_hold() {
    // **DEVICE_UI is explicit**: "speaking at any point keeps it a PTT hold". A person mid-sentence
    // who is shown a skin carousel has been interrupted by their own device.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000},
        {true,  kCheekX, kCheekY, 2500},
        {false, kCheekX, kCheekY, 2600},
    }, /*speech=*/true);

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kLongPress),
                          static_cast<int>(out[0].gesture));
}

static void test_a_hold_reports_at_most_one_thing() {
    // Without the latch a stroke would fire on every sample past the travel threshold, and the
    // reflex behind it would run dozens of times for one movement.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  80,  kCheekY, 1000},
        {true,  180, kCheekY, 1100},
        {true,  220, kCheekY, 1200},
        {true,  260, kCheekY, 1300},
        {false, 260, kCheekY, 1400},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
}

static void test_reset_drops_a_run_of_taps() {
    // A run that spanned a reply would report a count nobody performed.
    TouchGestures gestures;
    gestures.feed({true, kCheekX, kCheekY, 1000});
    gestures.feed({false, kCheekX, kCheekY, 1050});
    gestures.reset();

    const auto after = replay(gestures, {
        {true, kCheekX, kCheekY, 1200}, {false, kCheekX, kCheekY, 1250},
    });
    TEST_ASSERT_EQUAL_INT(1, after[0].count);
}

// ---------------------------------------------------------------------------------------
// Control is not affection (code review #2, #3)
// ---------------------------------------------------------------------------------------

static void test_two_fingers_are_not_affection() {
    // **The gesture that muted the microphone and delighted the character.** The panel reports only
    // the first touch through `getDetail()`, so a two-finger mute also read as an ordinary tap:
    // the face tickled and the server was told it had been petted.
    //
    // DEVICE_UI §Input separates affection from control and says control is local UI, deliberately
    // not reported. This is that line, enforced.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000, 2},
        {false, kCheekX, kCheekY, 1060, 2},
    });

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(out.size()));
}

static void test_a_second_finger_cancels_a_gesture_in_progress() {
    // Not merely ignored: a gesture half-formed when the control gesture began must not complete
    // after it ends, or muting would be followed by a stray tap.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000, 1},
        {true,  kCheekX, kCheekY, 1030, 2},
        {false, kCheekX, kCheekY, 1080, 1},
    });

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(out.size()));
}

static void test_one_finger_is_still_affection() {
    // The other half: the default must not have become "ignore everything".
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000, 1},
        {false, kCheekX, kCheekY, 1060, 1},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(out[0].gesture));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_outside_the_face_is_not_affection);
    RUN_TEST(test_an_eye_is_where_the_renderer_draws_one);
    RUN_TEST(test_moving_the_eyes_moves_the_poke_zone);
    RUN_TEST(test_forehead_and_cheek_split_at_the_eyes);
    RUN_TEST(test_a_tap_is_a_tap);
    RUN_TEST(test_taps_in_quick_succession_accumulate);
    RUN_TEST(test_taps_far_apart_do_not_accumulate);
    RUN_TEST(test_a_slow_drag_across_the_face_is_a_stroke);
    RUN_TEST(test_a_stroke_is_reported_while_it_happens);
    RUN_TEST(test_a_hold_that_wobbles_is_not_a_stroke);
    RUN_TEST(test_a_press_on_an_eye_is_a_poke);
    RUN_TEST(test_poking_an_eye_twice_is_still_a_poke);
    RUN_TEST(test_a_hold_past_the_ptt_threshold_is_a_long_press);
    RUN_TEST(test_a_hold_past_1200ms_without_speech_becomes_the_carousels);
    RUN_TEST(test_speaking_keeps_a_long_hold_a_ptt_hold);
    RUN_TEST(test_a_hold_reports_at_most_one_thing);
    RUN_TEST(test_reset_drops_a_run_of_taps);
    RUN_TEST(test_two_fingers_are_not_affection);
    RUN_TEST(test_a_second_finger_cancels_a_gesture_in_progress);
    RUN_TEST(test_one_finger_is_still_affection);
    return UNITY_END();
}

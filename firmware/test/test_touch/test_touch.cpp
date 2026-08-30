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


// ---------------------------------------------------------------------------------------
// Code review #1 — another consumer takes the finger
// ---------------------------------------------------------------------------------------

void test_forgetting_a_press_leaves_the_next_one_clean() {
    // **The defect this method exists for.** The carousel opens while the finger is still down and
    // then swallows the release. Without `forget()`, `held_` stays true, so the *next* touch is
    // never re-captured: it is measured from where the previous hold began, in the previous hold's
    // zone -- and a distance past `kStrokeTravelPx` caresses the face on behalf of nobody.
    roboface::TouchGestures gestures;
    gestures.feed({true, 40, 40, 1000});   // a hold begins near one corner of the face
    gestures.feed({true, 40, 40, 2300});   // ... long enough to be the carousel's
    gestures.forget();                     // the carousel takes the finger

    // The person lifts (never seen by the classifier), then taps somewhere else entirely.
    gestures.feed({true, kCheekX, kCheekY, 5000});
    const auto tap = gestures.feed({false, kCheekX, kCheekY, 5060});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(tap.gesture));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kCheek), static_cast<int>(tap.zone));
    TEST_ASSERT_EQUAL_UINT8(1, tap.count);
}

void test_forget_is_not_reset() {
    // Two methods because they answer two questions, and v2.4 narrowed `reset()` on purpose. A
    // press that spans a state change must survive; a press another consumer has taken must not.
    roboface::TouchGestures gestures;

    gestures.feed({true, kCheekX, kCheekY, 1000});
    gestures.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap),
                          static_cast<int>(gestures.feed({false, kCheekX, kCheekY, 1080}).gesture));

    gestures.feed({true, kCheekX, kCheekY, 3000});
    gestures.forget();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kNone),
                          static_cast<int>(gestures.feed({false, kCheekX, kCheekY, 3080}).gesture));
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Zones follow the face
// ---------------------------------------------------------------------------------------

static void test_outside_the_face_is_not_affection() {
    // The outer 28 px bands are chrome. A touch there belongs to the UI, not to the character.
    // (5,5) used to be here and is now the microphone button -- see the zone test below.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(160, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(315, 120)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside), static_cast<int>(roboface::zoneAt(5, 230)));
}

static void test_a_press_survives_a_state_change_under_the_finger() {
    // **The bug that made the button do nothing.** A press longer than the 120 ms PTT threshold
    // opens a listening window, the device changes state, and `reset()` used to clear `held_` --
    // so the release arrived at a classifier that had forgotten the press. Every deliberate press
    // evaporated; only a flick shorter than 120 ms ever produced a gesture.
    roboface::TouchGestures gestures;
    gestures.feed({true, kCheekX, kCheekY, 1000});
    gestures.reset();  // the state changed under the finger
    const auto result = gestures.feed({false, kCheekX, kCheekY, 1100});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(result.gesture));
}

static void test_a_state_change_still_drops_the_tap_run() {
    // And the finding `reset()` was written for is still fixed: a tap before a reply and one after
    // must not merge into a multi-tap nobody performed. The count goes; the press does not.
    roboface::TouchGestures gestures;
    gestures.feed({true, kCheekX, kCheekY, 1000});
    gestures.feed({false, kCheekX, kCheekY, 1050});

    gestures.reset();  // a reply happened

    gestures.feed({true, kCheekX, kCheekY, 1300});   // within kMultiTapWindowMs of the first
    const auto second = gestures.feed({false, kCheekX, kCheekY, 1350});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(second.gesture));
    TEST_ASSERT_EQUAL_UINT8(1, second.count);
}

static void test_the_button_survives_it_too() {
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    gestures.reset();
    const auto result = gestures.feed({false, 10, 10, 1200});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMicToggle), static_cast<int>(result.gesture));
}

static void test_the_microphone_button_owns_the_top_left_corner() {
    // The corner itself, the glyph, and the right-hand edge of the target.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton), static_cast<int>(roboface::zoneAt(0, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton), static_cast<int>(roboface::zoneAt(5, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton),
                          static_cast<int>(roboface::zoneAt(roboface::kMicButtonHitWidth - 1, 0)));

    // And it stops. A target that crept past its own width would eat the top band.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside),
                          static_cast<int>(roboface::zoneAt(roboface::kMicButtonHitWidth, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kOutside),
                          static_cast<int>(roboface::zoneAt(5, roboface::kMicButtonHitHeight)));
}

static void test_the_button_never_draws_over_the_face() {
    // The **glyph** is what DEVICE_UI's "never block the face" governs. The touch target is a
    // separate rectangle and deliberately reaches past this one -- see the test below.
    TEST_ASSERT_TRUE(roboface::clearOfFace(roboface::kMicButtonLeft, roboface::kMicButtonTop,
                                           roboface::kMicButtonWidth, roboface::kMicButtonHeight));
}

static void test_the_whole_target_answers_as_the_button() {
    for (int x = 0; x < roboface::kMicButtonHitWidth; ++x) {
        for (int y = 0; y < roboface::kMicButtonHitHeight; ++y) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton),
                                  static_cast<int>(roboface::zoneAt(x, y)));
        }
    }
}

static void test_the_target_reaches_below_the_band_on_purpose() {
    // **Measured, not guessed.** Eight presses aimed at the icon landed at y = 4, 7, 10, 10, 21,
    // 22, 29, 38 -- two of them past a 28 px band. A target one fingertip high is a target that
    // has to be aimed at, and this is a mute button.
    TEST_ASSERT_TRUE(roboface::kMicButtonHitHeight > roboface::kBandHeight);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton), static_cast<int>(roboface::zoneAt(31, 38)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton), static_cast<int>(roboface::zoneAt(42, 29)));

    // And it still ends. The face keeps everything below it.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kForehead),
                          static_cast<int>(roboface::zoneAt(42, roboface::kMicButtonHitHeight)));
}

static void test_the_button_takes_only_a_sliver_of_the_face() {
    // The cost, pinned so it cannot grow quietly. Anything beyond the button's width, or below its
    // height, is still the character's.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kForehead),
                          static_cast<int>(roboface::zoneAt(roboface::kMicButtonHitWidth, 40)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kForehead),
                          static_cast<int>(roboface::zoneAt(160, 40)));
}

static void test_tapping_the_button_is_the_toggle_and_not_affection() {
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    const auto result = gestures.feed({false, 10, 10, 1080});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMicToggle), static_cast<int>(result.gesture));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchZone::kMicButton), static_cast<int>(result.zone));
}

static void test_the_button_does_not_feed_the_affection_run() {
    // **The regression this file exists for.** Mute was the two-finger tap and then the double tap;
    // both leaked into affection -- the character was delighted by being silenced. A control that
    // shares a counter with affection will always find its way back into it, so the counter must
    // not see the button at all.
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    gestures.feed({false, 10, 10, 1050});   // the button

    gestures.feed({true, kCheekX, kCheekY, 2000});  // then a genuine tap on the face, long after
    const auto face = gestures.feed({false, kCheekX, kCheekY, 2050});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(face.gesture));
    TEST_ASSERT_EQUAL_UINT8(1, face.count);  // not 2: the button was never a tap
}

static void test_sliding_off_the_button_cancels_it() {
    // The only way to change your mind after touching a control that mutes you.
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    gestures.feed({true, kCheekX, kCheekY, 1100});
    const auto result = gestures.feed({false, kCheekX, kCheekY, 1150});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kNone), static_cast<int>(result.gesture));
}

static void test_the_button_is_never_a_stroke_or_a_carousel_hold() {
    // Nothing fires *while* it is held -- no stroke, no carousel -- and the release is still the
    // toggle however long the press lasted. A button that ignored a slow press would be a button
    // that ignored a careful person.
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    for (uint32_t t = 1050; t <= 3000; t += 50) {
        const auto held = gestures.feed({true, 10, 10, t});
        TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kNone), static_cast<int>(held.gesture));
    }
    const auto released = gestures.feed({false, 10, 10, 3050});
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMicToggle),
                          static_cast<int>(released.gesture));
}

static void test_a_deliberate_press_is_not_too_slow_for_the_button() {
    // 200 ms: what a person actually does when pressing a control they are looking at. The first
    // version of this button required < 120 ms -- the push-to-talk threshold, borrowed for no
    // reason -- and so rejected every real press.
    roboface::TouchGestures gestures;
    gestures.feed({true, 10, 10, 1000});
    const auto result = gestures.feed({false, 10, 10, 1200});

    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMicToggle), static_cast<int>(result.gesture));
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
    // DEVICE_UI: "repeated taps build joy". The count is what builds it. Spaced past the refractory
    // period so every tap is also reported -- the throttling has its own tests below.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  kCheekX, kCheekY, 1000}, {false, kCheekX, kCheekY, 1050},
        {true,  kCheekX, kCheekY, 1320}, {false, kCheekX, kCheekY, 1360},
        {true,  kCheekX, kCheekY, 1640}, {false, kCheekX, kCheekY, 1680},
    });

    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(out.size()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kTap), static_cast<int>(out[0].gesture));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::kMultiTap), static_cast<int>(out[1].gesture));
    TEST_ASSERT_EQUAL_INT(3, out[2].count);
}

//: **A burst is throttled, not discarded.**
//:
//: Drumming on the face produced a reflex and a wire frame per tap, which is more reaction than
//: anyone wants and more traffic than the event is worth. The count still rises through the burst,
//: so what comes out is fewer reactions, each stronger -- rather than the same one repeated.
void a_burst_of_taps_is_throttled() {
    TouchGestures gestures;
    std::vector<TouchSample> drumming;
    for (uint32_t t = 1000; t < 2000; t += 80) {
        drumming.push_back({true, kCheekX, kCheekY, t});
        drumming.push_back({false, kCheekX, kCheekY, t + 40});
    }

    std::vector<roboface::TouchResult> out;
    for (const auto& sample : drumming) {
        const auto result = gestures.feed(sample);
        if (result.gesture != TouchGesture::kNone) out.push_back(result);
    }

    // A second of drumming is a dozen taps; at one report per 250 ms it is four or five.
    TEST_ASSERT_TRUE(out.size() >= 3);
    TEST_ASSERT_TRUE(out.size() <= 5);
}

void a_throttled_burst_still_builds_the_count() {
    // The taps that were not reported were still counted -- which is what makes the throttling a
    // rate limit rather than a loss.
    TouchGestures gestures;
    std::vector<roboface::TouchResult> out;
    for (uint32_t t = 1000; t < 2000; t += 80) {
        gestures.feed({true, kCheekX, kCheekY, t});
        const auto result = gestures.feed({false, kCheekX, kCheekY, t + 40});
        if (result.gesture != TouchGesture::kNone) out.push_back(result);
    }

    TEST_ASSERT_TRUE(out.back().count > out.front().count + 2);
}

void the_first_touch_is_never_withheld() {
    // A refractory period that applied before anything had been reported would swallow the very
    // first tap after a boot, which is the one most likely to be someone checking the device works.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true, kCheekX, kCheekY, 50}, {false, kCheekX, kCheekY, 90},
    });

    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(out.size()));
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

    // Spaced past the refractory period: this test is about the *kind* surviving a second poke,
    // not about the throttling, which has its own tests.
    TouchGestures gestures;
    const auto out = replay(gestures, {
        {true,  eye_x, eye_y, 1000}, {false, eye_x, eye_y, 1050},
        {true,  eye_x, eye_y, 1320}, {false, eye_x, eye_y, 1360},
    });

    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(out.size()));
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
    RUN_TEST(test_a_press_survives_a_state_change_under_the_finger);
    RUN_TEST(test_forgetting_a_press_leaves_the_next_one_clean);
    RUN_TEST(test_forget_is_not_reset);
    RUN_TEST(test_a_state_change_still_drops_the_tap_run);
    RUN_TEST(test_the_button_survives_it_too);
    RUN_TEST(test_the_microphone_button_owns_the_top_left_corner);
    RUN_TEST(test_the_button_never_draws_over_the_face);
    RUN_TEST(test_the_whole_target_answers_as_the_button);
    RUN_TEST(test_the_target_reaches_below_the_band_on_purpose);
    RUN_TEST(test_the_button_takes_only_a_sliver_of_the_face);
    RUN_TEST(test_tapping_the_button_is_the_toggle_and_not_affection);
    RUN_TEST(test_the_button_does_not_feed_the_affection_run);
    RUN_TEST(test_sliding_off_the_button_cancels_it);
    RUN_TEST(test_the_button_is_never_a_stroke_or_a_carousel_hold);
    RUN_TEST(test_a_deliberate_press_is_not_too_slow_for_the_button);
    RUN_TEST(test_an_eye_is_where_the_renderer_draws_one);
    RUN_TEST(test_moving_the_eyes_moves_the_poke_zone);
    RUN_TEST(test_forehead_and_cheek_split_at_the_eyes);
    RUN_TEST(test_a_tap_is_a_tap);
    RUN_TEST(test_taps_in_quick_succession_accumulate);
    RUN_TEST(a_burst_of_taps_is_throttled);
    RUN_TEST(a_throttled_burst_still_builds_the_count);
    RUN_TEST(the_first_touch_is_never_withheld);
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

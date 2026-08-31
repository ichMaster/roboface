// v2.6 RF-082 — choosing a face with one finger, and who owns the bottom band.
//
// **The gesture costs something, and that is what these tests are mostly about.** Opening the
// carousel means a listening window was opened and then taken away: the person held the screen, the
// microphone came on, and 1.2 s later the device decided they meant something else. So the tests
// that matter are the ones about *not* stealing it, and about making a mistaken open cheap to
// escape.

#include <unity.h>

#include "pure/carousel.h"
#include "pure/chrome.h"
#include "pure/skin.h"

using roboface::BandTenant;
using roboface::Carousel;
using roboface::CarouselOutcome;
using roboface::carouselDotAt;
using roboface::carouselDotX;

namespace {

constexpr std::size_t kSkins = roboface::kSkinCount;

//: The y of the strip, so a test does not have to know the band's arithmetic.
constexpr int kStripY = roboface::kCarouselCentreY;

}  // namespace

// ---------------------------------------------------------------------------------------
// The strip's geometry
// ---------------------------------------------------------------------------------------

static void test_the_strip_is_centred_and_evenly_spaced() {
    // Symmetric about the screen's midline: the first and last dots are the same distance from it.
    const int first = carouselDotX(0, kSkins);
    const int last = carouselDotX(kSkins - 1, kSkins);
    TEST_ASSERT_EQUAL_INT(roboface::kScreenWidth / 2 - first, last - roboface::kScreenWidth / 2);

    for (std::size_t i = 1; i < kSkins; ++i) {
        TEST_ASSERT_EQUAL_INT(roboface::kCarouselDotSpacing,
                              carouselDotX(i, kSkins) - carouselDotX(i - 1, kSkins));
    }
}

static void test_the_strip_never_reaches_the_face() {
    // The band is chrome and the face is not, and the carousel is drawn in the band.
    TEST_ASSERT_TRUE(kStripY >= roboface::kFaceBottom);
    TEST_ASSERT_TRUE(kStripY < roboface::kScreenHeight);
}

static void test_a_dot_answers_where_it_is_drawn() {
    // **The property `touch.h` established and this repeats**: the hit test and the pixels come
    // from one number, so moving the strip moves both. Written the other way, a spacing change
    // makes the dots and their targets drift apart, and the symptom is a carousel that chooses the
    // neighbour of whatever you pressed.
    for (std::size_t i = 0; i < kSkins; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i, carouselDotAt(carouselDotX(i, kSkins), kStripY, kSkins));
    }
}

static void test_a_press_between_two_dots_takes_the_nearer() {
    const int between = (carouselDotX(1, kSkins) + carouselDotX(2, kSkins)) / 2;
    TEST_ASSERT_EQUAL_UINT32(2, carouselDotAt(between + 4, kStripY, kSkins));
    TEST_ASSERT_EQUAL_UINT32(1, carouselDotAt(between - 4, kStripY, kSkins));
}

static void test_well_above_the_strip_is_outside_it() {
    TEST_ASSERT_EQUAL_UINT32(kSkins, carouselDotAt(roboface::kScreenWidth / 2, 60, kSkins));
}

static void test_far_to_either_side_is_outside_it() {
    TEST_ASSERT_EQUAL_UINT32(kSkins, carouselDotAt(2, kStripY, kSkins));
    TEST_ASSERT_EQUAL_UINT32(kSkins, carouselDotAt(roboface::kScreenWidth - 2, kStripY, kSkins));
}

// ---------------------------------------------------------------------------------------
// Choosing — by tapping, since v2.6.2
// ---------------------------------------------------------------------------------------

namespace {

//: A carousel already open and past the release of the press that opened it, which is where every
//: test about *choosing* wants to start.
Carousel ready(std::size_t current) {
    Carousel carousel;
    carousel.open(current, kSkins, 1000);
    carousel.released();
    return carousel;
}

//: Coordinates for each zone, taken from the layout rather than typed.
constexpr int kPrevX = roboface::kFaceLeft + 10;
constexpr int kNextX = roboface::kFaceRight - 10;
constexpr int kMidX = roboface::kScreenWidth / 2;
constexpr int kMidY = roboface::kFaceTop + roboface::kFaceHeight / 2;

}  // namespace

static void test_the_zones_cover_every_pixel() {
    // **Total over the screen.** A modal picker that let some taps fall through would tickle the
    // face from behind its own arrows -- the leak the mute gesture kept springing, in a new place.
    for (int x = 0; x < roboface::kScreenWidth; x += 7) {
        for (int y = 0; y < roboface::kScreenHeight; y += 7) {
            TEST_ASSERT_NOT_EQUAL(static_cast<int>(roboface::CarouselZone::kNone),
                                  static_cast<int>(roboface::carouselZoneAt(x, y, kSkins)));
        }
    }
}

static void test_every_target_is_bigger_than_a_fingertip() {
    // The measurement that forced this redesign: the old strip asked for a 5 px dot, and on the
    // board that was called stiff. A fingertip is about 24 px.
    TEST_ASSERT_TRUE(roboface::kCarouselArrowWidth >= 70);
    TEST_ASSERT_TRUE(roboface::kCarouselNextLeft - roboface::kCarouselPrevRight >= 70);
}

static void test_opening_starts_on_the_face_already_worn() {
    Carousel carousel;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kOpened),
                          static_cast<int>(carousel.open(3, kSkins, 1000)));
    TEST_ASSERT_EQUAL_UINT32(3, carousel.selected());
    TEST_ASSERT_TRUE(carousel.isOpen());
}

static void test_the_press_that_opened_it_does_not_also_confirm_it() {
    // **The bug this guard exists for.** The gesture is a hold *on the face*, which is the confirm
    // zone -- so without it every carousel would close on the release of the press that opened it,
    // in the same frame, and nobody would ever see the strip.
    Carousel carousel;
    carousel.open(0, kSkins, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.tapped(kMidX, kMidY, 1100)));
    TEST_ASSERT_TRUE(carousel.isOpen());

    carousel.released();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kConfirmed),
                          static_cast<int>(carousel.tapped(kMidX, kMidY, 1200)));
}

static void test_the_arrows_step_and_wrap() {
    // Wrapping both ways, because with five faces and no wrap the person at one end has to travel
    // the whole strip to reach the other -- and with a picker this small the ends are where you
    // most often are.
    Carousel carousel = ready(0);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kMoved),
                          static_cast<int>(carousel.tapped(kPrevX, kMidY, 1100)));
    TEST_ASSERT_EQUAL_UINT32(kSkins - 1, carousel.selected());

    carousel.tapped(kNextX, kMidY, 1200);
    TEST_ASSERT_EQUAL_UINT32(0, carousel.selected());

    carousel.tapped(kNextX, kMidY, 1300);
    TEST_ASSERT_EQUAL_UINT32(1, carousel.selected());
}

static void test_a_dot_jumps_straight_to_its_face() {
    // The dots stay because a strip with one lit is the only thing that says *where you are* -- but
    // nothing requires hitting one any more, and hitting one is a shortcut rather than the method.
    Carousel carousel = ready(0);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kMoved),
                          static_cast<int>(carousel.tapped(carouselDotX(3, kSkins),
                                                           kStripY, 1100)));
    TEST_ASSERT_EQUAL_UINT32(3, carousel.selected());
}

static void test_tapping_the_dot_already_lit_changes_nothing() {
    Carousel carousel = ready(2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.tapped(carouselDotX(2, kSkins),
                                                           kStripY, 1100)));
    TEST_ASSERT_TRUE(carousel.isOpen());
}

static void test_tapping_the_face_confirms_it() {
    Carousel carousel = ready(0);
    carousel.tapped(kNextX, kMidY, 1100);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kConfirmed),
                          static_cast<int>(carousel.tapped(kMidX, kMidY, 1200)));
    TEST_ASSERT_EQUAL_UINT32(1, carousel.selected());
    TEST_ASSERT_FALSE(carousel.isOpen());
}

static void test_the_top_band_cancels_and_restores() {
    // **The escape**, and it is a large target on purpose: the gesture already cost a listening
    // window, so changing your mind must not be the hardest part.
    Carousel carousel = ready(4);
    carousel.tapped(kPrevX, kMidY, 1100);
    TEST_ASSERT_EQUAL_UINT32(3, carousel.selected());  // previewed

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kCancelled),
                          static_cast<int>(carousel.tapped(kMidX, 8, 1200)));
    TEST_ASSERT_EQUAL_UINT32(4, carousel.selected());  // the face that was worn
    TEST_ASSERT_FALSE(carousel.isOpen());
}

static void test_an_untouched_picker_gives_up() {
    // A picker that stays open forever is a device that has stopped being a companion: the gesture
    // can be performed by accident, and the face is buried under arrows until someone deals with it.
    Carousel carousel = ready(2);
    carousel.tapped(kNextX, kMidY, 1100);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.tick(1100 + roboface::kCarouselIdleMs - 1)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kCancelled),
                          static_cast<int>(carousel.tick(1100 + roboface::kCarouselIdleMs)));
    TEST_ASSERT_EQUAL_UINT32(2, carousel.selected());  // restored, not the preview
}

static void test_every_tap_postpones_the_timeout() {
    Carousel carousel = ready(0);
    uint32_t clock = 1000;
    for (int i = 0; i < 10; ++i) {
        clock += roboface::kCarouselIdleMs - 500;
        carousel.tapped(kNextX, kMidY, clock);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                              static_cast<int>(carousel.tick(clock + 10)));
    }
    TEST_ASSERT_TRUE(carousel.isOpen());
}

static void test_being_abandoned_restores_rather_than_keeping_the_preview() {
    Carousel carousel = ready(0);
    carousel.tapped(kNextX, kMidY, 1100);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kCancelled),
                          static_cast<int>(carousel.abandon()));
    TEST_ASSERT_EQUAL_UINT32(0, carousel.selected());
    TEST_ASSERT_FALSE(carousel.isOpen());
}

static void test_a_closed_carousel_ignores_everything() {
    Carousel carousel;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.tapped(kMidX, kMidY, 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.tick(999999)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.abandon()));
}

static void test_opening_twice_is_not_a_second_open() {
    Carousel carousel = ready(2);
    carousel.tapped(carouselDotX(0, kSkins), kStripY, 1100);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CarouselOutcome::kNothing),
                          static_cast<int>(carousel.open(2, kSkins, 2000)));
    TEST_ASSERT_EQUAL_UINT32(0, carousel.selected());
}

// ---------------------------------------------------------------------------------------
// The band
// ---------------------------------------------------------------------------------------

static void test_the_carousel_outranks_everything() {
    roboface::Chrome chrome;
    roboface::ChromeFacts facts;
    facts.carousel_wanted = true;
    facts.fault_active = true;
    facts.level_meter_wanted = true;
    facts.toast_until_ms = 9000;
    chrome.update(1000, facts);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kCarousel),
                          static_cast<int>(chrome.visibility().band));
}

static void test_a_fault_outranks_a_toast() {
    // ROADMAP orders "carousel > toast > level meter" and says nothing about the fault, because the
    // fault's rule is stated separately and is stronger than any of them: the one thing a person
    // must not miss is the one thing that cannot disappear on its own.
    roboface::Chrome chrome;
    roboface::ChromeFacts facts;
    facts.fault_active = true;
    facts.toast_until_ms = 9000;
    chrome.update(1000, facts);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kFault),
                          static_cast<int>(chrome.visibility().band));
}

static void test_a_toast_outranks_the_meter_and_then_expires() {
    roboface::Chrome chrome;
    roboface::ChromeFacts facts;
    facts.level_meter_wanted = true;
    facts.toast_until_ms = 3000;

    chrome.update(1000, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kToast),
                          static_cast<int>(chrome.visibility().band));

    // **An expiry, not a flag.** A boolean someone must remember to clear is a boolean that stays
    // set, and a toast that never went away would hold the band against the meter all session.
    chrome.update(3001, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kLevel),
                          static_cast<int>(chrome.visibility().band));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_strip_is_centred_and_evenly_spaced);
    RUN_TEST(test_the_strip_never_reaches_the_face);
    RUN_TEST(test_a_dot_answers_where_it_is_drawn);
    RUN_TEST(test_a_press_between_two_dots_takes_the_nearer);
    RUN_TEST(test_well_above_the_strip_is_outside_it);
    RUN_TEST(test_far_to_either_side_is_outside_it);
    RUN_TEST(test_the_zones_cover_every_pixel);
    RUN_TEST(test_every_target_is_bigger_than_a_fingertip);
    RUN_TEST(test_opening_starts_on_the_face_already_worn);
    RUN_TEST(test_the_press_that_opened_it_does_not_also_confirm_it);
    RUN_TEST(test_the_arrows_step_and_wrap);
    RUN_TEST(test_a_dot_jumps_straight_to_its_face);
    RUN_TEST(test_tapping_the_dot_already_lit_changes_nothing);
    RUN_TEST(test_tapping_the_face_confirms_it);
    RUN_TEST(test_the_top_band_cancels_and_restores);
    RUN_TEST(test_an_untouched_picker_gives_up);
    RUN_TEST(test_every_tap_postpones_the_timeout);
    RUN_TEST(test_being_abandoned_restores_rather_than_keeping_the_preview);
    RUN_TEST(test_a_closed_carousel_ignores_everything);
    RUN_TEST(test_opening_twice_is_not_a_second_open);
    RUN_TEST(test_the_carousel_outranks_everything);
    RUN_TEST(test_a_fault_outranks_a_toast);
    RUN_TEST(test_a_toast_outranks_the_meter_and_then_expires);
    return UNITY_END();
}

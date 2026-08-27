// Host tests for press-and-hold.
//
// A fake clock, because the rules are about time. Proving "a 100 ms tap is not an utterance" on
// hardware means tapping the glass for exactly 100 ms; here it is an integer.

#include <unity.h>

#include "pure/ptt.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

void test_a_press_alone_starts_nothing() {
    PushToTalk ptt;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone), static_cast<int>(ptt.update(true, 0)));
    TEST_ASSERT_FALSE(ptt.isHolding());
}

void test_a_hold_past_the_threshold_starts_once() {
    // Once, not once per loop. A caller that sent listen_start on every loop while a finger was
    // down would open a window sixty times a second.
    PushToTalk ptt;
    ptt.update(true, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone), static_cast<int>(ptt.update(true, 100)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kStarted),
                          static_cast<int>(ptt.update(true, 120)));
    for (uint32_t t = 140; t < 2000; t += 20) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                              static_cast<int>(ptt.update(true, t)));
    }
    TEST_ASSERT_TRUE(ptt.isHolding());
}

void test_releasing_a_hold_stops_once() {
    PushToTalk ptt;
    ptt.update(true, 0);
    ptt.update(true, 120);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kStopped),
                          static_cast<int>(ptt.update(false, 900)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                          static_cast<int>(ptt.update(false, 920)));
    TEST_ASSERT_FALSE(ptt.isHolding());
}

void test_a_release_before_the_threshold_is_a_tap_not_an_utterance() {
    // The case this type exists for. A window that opened and closed in 80 ms would send
    // listen_start and listen_stop with nothing between them.
    PushToTalk ptt;
    ptt.update(true, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kTapped),
                          static_cast<int>(ptt.update(false, 80)));
    TEST_ASSERT_FALSE(ptt.isHolding());
}

void test_a_release_exactly_at_the_threshold_is_a_hold() {
    // The boundary is inclusive, and stated: a rule with an undefined edge gets a different answer
    // from whoever reads it next.
    PushToTalk ptt;
    ptt.update(true, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kStarted),
                          static_cast<int>(ptt.update(true, kPttHoldMs)));
}

void test_a_release_with_no_press_does_nothing() {
    PushToTalk ptt;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                          static_cast<int>(ptt.update(false, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                          static_cast<int>(ptt.update(false, 500)));
}

void test_taps_do_not_accumulate_into_a_hold() {
    // Five quick taps are five taps. Without resetting the press time on each press, the fifth
    // would look like a 400 ms hold.
    PushToTalk ptt;
    for (uint32_t t = 0; t < 500; t += 100) {
        ptt.update(true, t);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kTapped),
                              static_cast<int>(ptt.update(false, t + 50)));
    }
}

void test_a_second_hold_works_after_the_first() {
    PushToTalk ptt;
    ptt.update(true, 0);
    ptt.update(true, 200);
    ptt.update(false, 500);

    ptt.update(true, 1000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kStarted),
                          static_cast<int>(ptt.update(true, 1200)));
}

void test_cancel_abandons_a_hold_without_reporting_a_stop() {
    // For a fault or a disconnect: the window is already gone, and telling the caller to close it
    // again would be a second close on something that no longer exists.
    PushToTalk ptt;
    ptt.update(true, 0);
    ptt.update(true, 200);
    TEST_ASSERT_TRUE(ptt.isHolding());

    ptt.cancel();
    TEST_ASSERT_FALSE(ptt.isHolding());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                          static_cast<int>(ptt.update(false, 300)));
}

void test_a_custom_threshold_is_respected() {
    PushToTalk ptt(500);
    ptt.update(true, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kNone),
                          static_cast<int>(ptt.update(true, 400)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PttEvent::kStarted),
                          static_cast<int>(ptt.update(true, 500)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_press_alone_starts_nothing);
    RUN_TEST(test_a_hold_past_the_threshold_starts_once);
    RUN_TEST(test_releasing_a_hold_stops_once);
    RUN_TEST(test_a_release_before_the_threshold_is_a_tap_not_an_utterance);
    RUN_TEST(test_a_release_exactly_at_the_threshold_is_a_hold);
    RUN_TEST(test_a_release_with_no_press_does_nothing);
    RUN_TEST(test_taps_do_not_accumulate_into_a_hold);
    RUN_TEST(test_a_second_hold_works_after_the_first);
    RUN_TEST(test_cancel_abandons_a_hold_without_reporting_a_stop);
    RUN_TEST(test_a_custom_threshold_is_respected);
    return UNITY_END();
}

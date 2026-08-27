// Host tests for the chrome visibility rules.
//
// These rules are almost entirely about time, and an injected clock is what makes them testable at
// all: every assertion below would otherwise mean watching a screen for four seconds and trusting
// what you saw. Here the three-second fade is proved in microseconds, and the "a fault never
// auto-dismisses" rule is proved by advancing an hour.

#include <unity.h>

#include "pure/chrome.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static ChromeFacts connectedAndHealthy() {
    ChromeFacts facts;
    facts.link = LinkState::kConnected;
    facts.battery_percent = 80;
    facts.charging = false;
    facts.fault_active = false;
    return facts;
}

// ---------------------------------------------------------------------------------------
// Link — appears when it is not simply working, then settles and hides
// ---------------------------------------------------------------------------------------

static void test_link_is_visible_while_connecting() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.link = LinkState::kConnecting;
    chrome.update(1000, facts);

    TEST_ASSERT_TRUE(chrome.visibility().link);
}

static void test_link_is_visible_while_offline_or_degraded() {
    for (const auto state : {LinkState::kOffline, LinkState::kDegraded}) {
        Chrome chrome;
        ChromeFacts facts = connectedAndHealthy();
        facts.link = state;
        chrome.update(1000, facts);
        // No timer: a link that is not working is not news that goes stale.
        chrome.update(1000 + 60 * 60 * 1000, facts);

        TEST_ASSERT_TRUE(chrome.visibility().link);
    }
}

static void test_link_hides_about_three_seconds_after_it_settles_connected() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.link = LinkState::kConnecting;
    chrome.update(0, facts);

    facts.link = LinkState::kConnected;
    chrome.update(1000, facts);
    TEST_ASSERT_TRUE_MESSAGE(chrome.visibility().link, "should still be shown right after settling");

    chrome.update(1000 + kSettleHideMs - 1, facts);
    TEST_ASSERT_TRUE_MESSAGE(chrome.visibility().link, "should still be shown just before the fade");

    chrome.update(1000 + kSettleHideMs + 1, facts);
    TEST_ASSERT_FALSE_MESSAGE(chrome.visibility().link, "a working link is not news");
}

static void test_a_change_while_fading_restarts_the_timer() {
    // The property that matters when a link flaps: without it the indicator keeps counting down
    // through the changes and blinks itself invisible -- least visible exactly when it matters
    // most.
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    chrome.update(0, facts);
    chrome.update(kSettleHideMs - 100, facts);
    TEST_ASSERT_TRUE(chrome.visibility().link);

    facts.link = LinkState::kDegraded;
    chrome.update(kSettleHideMs - 50, facts);
    facts.link = LinkState::kConnected;
    chrome.update(kSettleHideMs, facts);

    // The clock is already past the original deadline, but the timer restarted at the change.
    chrome.update(kSettleHideMs + 100, facts);
    TEST_ASSERT_TRUE_MESSAGE(chrome.visibility().link, "the flap should have restarted the timer");
}

// ---------------------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------------------

static void test_battery_is_hidden_when_it_is_not_worth_knowing() {
    Chrome chrome;
    chrome.update(0, connectedAndHealthy());

    TEST_ASSERT_FALSE(chrome.visibility().battery);
}

static void test_battery_appears_below_twenty_percent() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.battery_percent = kLowBatteryPercent - 1;
    chrome.update(0, facts);

    TEST_ASSERT_TRUE(chrome.visibility().battery);
}

static void test_battery_is_hidden_exactly_at_the_threshold() {
    // "below 20 %" -- 20 itself is not below 20. An off-by-one here is an indicator that appears
    // and disappears as the reading jitters across the boundary.
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.battery_percent = kLowBatteryPercent;
    chrome.update(0, facts);

    TEST_ASSERT_FALSE(chrome.visibility().battery);
}

static void test_battery_appears_while_charging_at_any_level() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.battery_percent = 95;
    facts.charging = true;
    chrome.update(0, facts);

    TEST_ASSERT_TRUE(chrome.visibility().battery);
}

// ---------------------------------------------------------------------------------------
// The rule the fading is safe because of
// ---------------------------------------------------------------------------------------

static void test_a_fault_never_auto_dismisses() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.fault_active = true;
    facts.fault = ErrorCode::kLlmFailed;
    chrome.update(0, facts);

    // An hour later.
    chrome.update(60 * 60 * 1000, facts);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kFault),
                          static_cast<int>(chrome.visibility().band));
    TEST_ASSERT_TRUE(chrome.faultActive());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kLlmFailed), static_cast<int>(chrome.fault()));
}

static void test_clearing_a_fault_frees_the_band() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.fault_active = true;
    chrome.update(0, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kFault), static_cast<int>(chrome.band()));

    facts.fault_active = false;
    chrome.update(1000, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kNothing), static_cast<int>(chrome.band()));
}

// ---------------------------------------------------------------------------------------
// The bottom band is single-tenant
// ---------------------------------------------------------------------------------------

static void test_the_band_yields_exactly_one_tenant() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.fault_active = true;
    facts.level_meter_wanted = true;
    facts.carousel_wanted = true;
    chrome.update(0, facts);

    // Three want it; one gets it. A band that grew tenants without a rule would draw them on top
    // of each other, which is how a level meter ends up illegible behind an error code.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kCarousel), static_cast<int>(chrome.band()));
}

static void test_band_priority_is_carousel_then_fault_then_level() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();

    facts.level_meter_wanted = true;
    chrome.update(0, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kLevel), static_cast<int>(chrome.band()));

    facts.fault_active = true;
    chrome.update(1, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kFault), static_cast<int>(chrome.band()));

    facts.carousel_wanted = true;
    chrome.update(2, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kCarousel), static_cast<int>(chrome.band()));
}

// ---------------------------------------------------------------------------------------
// The documented timings
// ---------------------------------------------------------------------------------------

static void test_the_fade_timings_are_the_documented_ones() {
    // DEVICE_UI §Motion and timing. Named here so RF-021 draws to these rather than inventing its
    // own, and so changing one changes it everywhere.
    TEST_ASSERT_EQUAL_UINT32(120u, kChromeFadeInMs);
    TEST_ASSERT_EQUAL_UINT32(400u, kChromeFadeOutMs);
    TEST_ASSERT_EQUAL_UINT32(3000u, kSettleHideMs);
    TEST_ASSERT_EQUAL_INT(20, kLowBatteryPercent);
}

// ---------------------------------------------------------------------------------------
// The facts reach the drawing code
// ---------------------------------------------------------------------------------------

static void test_the_facts_are_readable_for_drawing() {
    // Without these the chrome view had to invent values, and did: it drew "connected" arcs and an
    // empty battery whatever the truth was. Two places holding the same facts is how a pill ends
    // up showing a level the fade logic has already decided is stale.
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    facts.link = LinkState::kOffline;
    facts.battery_percent = 7;
    facts.charging = true;
    chrome.update(0, facts);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::kOffline), static_cast<int>(chrome.link()));
    TEST_ASSERT_EQUAL_INT(7, chrome.batteryPercent());
    TEST_ASSERT_TRUE(chrome.charging());
}

static void test_the_reported_facts_track_updates() {
    Chrome chrome;
    ChromeFacts facts = connectedAndHealthy();
    chrome.update(0, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::kConnected), static_cast<int>(chrome.link()));

    facts.link = LinkState::kDegraded;
    facts.battery_percent = 42;
    chrome.update(100, facts);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::kDegraded), static_cast<int>(chrome.link()));
    TEST_ASSERT_EQUAL_INT(42, chrome.batteryPercent());
}

// --- the level meter as a real band tenant (v1.2) --------------------------------------
//
// v0.4 wrote the arbitration before anything contended for the band. The meter is the first real
// contender, so these check the rule holds now that it can actually fire.

static void test_the_meter_owns_the_band_while_listening() {
    Chrome chrome;
    ChromeFacts facts;
    facts.level_meter_wanted = true;
    chrome.update(1000, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kLevel),
                          static_cast<int>(chrome.visibility().band));
}

static void test_a_fault_outranks_the_meter() {
    // DEVICE_UI's priority, and the reason for it: the one thing a person must not miss cannot be
    // hidden by the one thing that is merely reassuring.
    Chrome chrome;
    ChromeFacts facts;
    facts.level_meter_wanted = true;
    facts.fault_active = true;
    chrome.update(1000, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kFault),
                          static_cast<int>(chrome.visibility().band));
}

static void test_the_carousel_outranks_both() {
    Chrome chrome;
    ChromeFacts facts;
    facts.level_meter_wanted = true;
    facts.fault_active = true;
    facts.carousel_wanted = true;
    chrome.update(1000, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kCarousel),
                          static_cast<int>(chrome.visibility().band));
}

static void test_the_band_empties_when_listening_ends() {
    // The meter is live, not settled: it goes the moment the window closes rather than fading
    // three seconds later like the link and battery indicators.
    Chrome chrome;
    ChromeFacts facts;
    facts.level_meter_wanted = true;
    chrome.update(1000, facts);

    facts.level_meter_wanted = false;
    chrome.update(1020, facts);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BandTenant::kNothing),
                          static_cast<int>(chrome.visibility().band));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_meter_owns_the_band_while_listening);
    RUN_TEST(test_a_fault_outranks_the_meter);
    RUN_TEST(test_the_carousel_outranks_both);
    RUN_TEST(test_the_band_empties_when_listening_ends);
    RUN_TEST(test_the_facts_are_readable_for_drawing);
    RUN_TEST(test_the_reported_facts_track_updates);
    RUN_TEST(test_link_is_visible_while_connecting);
    RUN_TEST(test_link_is_visible_while_offline_or_degraded);
    RUN_TEST(test_link_hides_about_three_seconds_after_it_settles_connected);
    RUN_TEST(test_a_change_while_fading_restarts_the_timer);
    RUN_TEST(test_battery_is_hidden_when_it_is_not_worth_knowing);
    RUN_TEST(test_battery_appears_below_twenty_percent);
    RUN_TEST(test_battery_is_hidden_exactly_at_the_threshold);
    RUN_TEST(test_battery_appears_while_charging_at_any_level);
    RUN_TEST(test_a_fault_never_auto_dismisses);
    RUN_TEST(test_clearing_a_fault_frees_the_band);
    RUN_TEST(test_the_band_yields_exactly_one_tenant);
    RUN_TEST(test_band_priority_is_carousel_then_fault_then_level);
    RUN_TEST(test_the_fade_timings_are_the_documented_ones);
    return UNITY_END();
}

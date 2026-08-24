// Host tests for the console mode's state discipline.
//
// The DoD clause is "`/chat-off` restores exactly the face and state that were showing before".
// That is checkable without a screen: what the mode gives back must be what it took, for every
// state it could have taken it from.

#include <unity.h>

#include "pure/console.h"
#include "pure/state.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

static const DeviceState kAllStates[] = {
    DeviceState::kBoot,      DeviceState::kWifiConnecting, DeviceState::kIdle,
    DeviceState::kListening, DeviceState::kThinking,       DeviceState::kReplying,
    DeviceState::kOffline,   DeviceState::kError,
};

void test_a_new_console_is_off() {
    const ConsoleMode console;
    TEST_ASSERT_FALSE(console.isOn());
}

void test_enabling_turns_it_on_and_reports_the_change() {
    ConsoleMode console;
    TEST_ASSERT_TRUE(console.enable(DeviceState::kIdle));
    TEST_ASSERT_TRUE(console.isOn());
}

void test_it_gives_back_every_state_it_can_take() {
    // Total over the enum, including boot and wifi_connecting: the console can be entered at any
    // moment, and a state it could not restore would be one it must not borrow from.
    for (const DeviceState state : kAllStates) {
        ConsoleMode console;
        console.enable(state);
        TEST_ASSERT_TRUE(console.disable());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(state), static_cast<int>(console.savedState()));
        TEST_ASSERT_FALSE(console.isOn());
    }
}

void test_entering_twice_keeps_the_first_saved_state() {
    // The second `/chat-on` arrives while the console owns the screen. Saving again would record
    // whatever the device is showing *now* and leaving would restore that instead of the real
    // previous state -- the bug being prevented, not a theoretical one.
    ConsoleMode console;
    console.enable(DeviceState::kOffline);
    TEST_ASSERT_FALSE(console.enable(DeviceState::kError));
    TEST_ASSERT_TRUE(console.isOn());
    console.disable();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kOffline),
                          static_cast<int>(console.savedState()));
}

void test_leaving_when_already_off_is_a_no_op() {
    ConsoleMode console;
    TEST_ASSERT_FALSE(console.disable());
    TEST_ASSERT_FALSE(console.isOn());
}

void test_it_can_be_entered_again_after_leaving() {
    ConsoleMode console;
    console.enable(DeviceState::kIdle);
    console.disable();
    TEST_ASSERT_TRUE(console.enable(DeviceState::kThinking));
    console.disable();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kThinking),
                          static_cast<int>(console.savedState()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_new_console_is_off);
    RUN_TEST(test_enabling_turns_it_on_and_reports_the_change);
    RUN_TEST(test_it_gives_back_every_state_it_can_take);
    RUN_TEST(test_entering_twice_keeps_the_first_saved_state);
    RUN_TEST(test_leaving_when_already_off_is_a_no_op);
    RUN_TEST(test_it_can_be_entered_again_after_leaving);
    return UNITY_END();
}

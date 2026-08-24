// Host tests for the console mode.
//
// What is worth asserting here is small but load-bearing: the mode is idempotent in both
// directions, so a repeated command cannot leave the screen owned by nobody or by two things.
//
// What this file deliberately does **not** test any more is state save/restore. The console used to
// save the device state on entry and put it back on exit; the v0.5 review found that reverts
// transitions which happened legitimately while the console was open. The console owns the screen,
// not the state, and `/chat-off` renders whatever state the device is actually in -- which is a
// property of `main.cpp`'s render path, not of this type.

#include <unity.h>

#include "pure/console.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

void test_a_new_console_is_off() {
    const ConsoleMode console;
    TEST_ASSERT_FALSE(console.isOn());
}

void test_enabling_turns_it_on_and_reports_the_change() {
    ConsoleMode console;
    TEST_ASSERT_TRUE(console.enable());
    TEST_ASSERT_TRUE(console.isOn());
}

void test_entering_twice_is_a_no_op() {
    ConsoleMode console;
    console.enable();
    TEST_ASSERT_FALSE(console.enable());
    TEST_ASSERT_TRUE(console.isOn());
}

void test_leaving_turns_it_off_and_reports_the_change() {
    ConsoleMode console;
    console.enable();
    TEST_ASSERT_TRUE(console.disable());
    TEST_ASSERT_FALSE(console.isOn());
}

void test_leaving_when_already_off_is_a_no_op() {
    ConsoleMode console;
    TEST_ASSERT_FALSE(console.disable());
    TEST_ASSERT_FALSE(console.isOn());
}

void test_it_can_be_entered_again_after_leaving() {
    ConsoleMode console;
    console.enable();
    console.disable();
    TEST_ASSERT_TRUE(console.enable());
    TEST_ASSERT_TRUE(console.isOn());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_new_console_is_off);
    RUN_TEST(test_enabling_turns_it_on_and_reports_the_change);
    RUN_TEST(test_entering_twice_is_a_no_op);
    RUN_TEST(test_leaving_turns_it_off_and_reports_the_change);
    RUN_TEST(test_leaving_when_already_off_is_a_no_op);
    RUN_TEST(test_it_can_be_entered_again_after_leaving);
    return UNITY_END();
}

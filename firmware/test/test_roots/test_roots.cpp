// v2.5 RF-074 — the square root that the private Newton loops got wrong.
//
// This file exists because `coherence()` reported **0.6 for two identical channels**. Its input is a
// product of two frame energies, routinely around 1e20, and the "start at the value, iterate 32
// times, break when it stops moving" pattern copied through this project does not converge from
// there: each step roughly halves the exponent's error, so it needs about thirty-five.
//
// The failure mode is the dangerous kind — no crash, no NaN, just a number quietly several orders of
// magnitude wrong, in the direction that makes a person look like a room.

#include <unity.h>

#include "pure/roots.h"

using roboface::squareRoot;

static void test_small_roots_are_exact_enough() {
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, squareRoot(4.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.0f, squareRoot(9.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, squareRoot(1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.41421f, squareRoot(2.0f));
}

static void test_the_range_that_broke_coherence() {
    // A frame of 320 samples near full scale carries about 1e11 of energy; two of them multiplied
    // is 1e22. This is the number the old loop could not reach.
    const float energy = 1.0e11f;
    const float product = energy * energy;
    TEST_ASSERT_FLOAT_WITHIN(energy * 0.001f, energy, squareRoot(product));
}

static void test_it_holds_across_twenty_orders_of_magnitude() {
    float value = 1.0e-6f;
    for (int i = 0; i < 14; ++i) {
        const float root = squareRoot(value);
        // The invariant, not a table: root * root == value, to float precision.
        TEST_ASSERT_FLOAT_WITHIN(value * 0.002f, value, root * root);
        value *= 100.0f;
    }
}

static void test_below_zero_is_zero_rather_than_a_nan() {
    // A negative energy means a caller computed something wrongly. Propagating a NaN would turn an
    // arithmetic mistake into a face that renders as nothing at all.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, squareRoot(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, squareRoot(-1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, squareRoot(-1.0e18f));
}

static void test_tiny_values_do_not_hang_the_scaling_loop() {
    // The `while (scaled < 1.0f)` half of the reduction, driven to its limit.
    TEST_ASSERT_TRUE(squareRoot(1.0e-30f) > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-16f, 1.0e-15f, squareRoot(1.0e-30f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_small_roots_are_exact_enough);
    RUN_TEST(test_the_range_that_broke_coherence);
    RUN_TEST(test_it_holds_across_twenty_orders_of_magnitude);
    RUN_TEST(test_below_zero_is_zero_rather_than_a_nan);
    RUN_TEST(test_tiny_values_do_not_hang_the_scaling_loop);
    return UNITY_END();
}

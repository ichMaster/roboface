// The IMU classifier, from motions built out of physics.
//
// Every fixture here is generated from what the accelerometer would actually read, not from numbers
// chosen to make an assertion pass. That distinction cost v2.3 a recalibration: a fixture that does
// not resemble the thing produces thresholds that pass every test and fail on the desk.
//
// So: at rest the sensor reads 1 g toward the floor. A tilt rotates that vector. A drop collapses
// its magnitude. A shake swings it past twice gravity and back. Each fixture below is that sentence,
// written out.

#include <unity.h>

#include <vector>

#include "pure/motion.h"

using roboface::AccelSample;
using roboface::Motion;
using roboface::MotionDetector;

namespace {

//: A device sitting on a desk: gravity straight down the z axis.
AccelSample resting(uint32_t at_ms) { return AccelSample{0.0f, 0.0f, 1.0f, at_ms}; }

//: Feed a sequence, keep what it produced.
std::vector<Motion> replay(MotionDetector& detector, const std::vector<AccelSample>& samples) {
    std::vector<Motion> out;
    for (const auto& sample : samples) {
        const Motion motion = detector.feed(sample);
        if (motion != Motion::kNone) out.push_back(motion);
    }
    return out;
}

bool contains(const std::vector<Motion>& motions, Motion wanted) {
    for (const Motion motion : motions) {
        if (motion == wanted) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The case that matters most
// ---------------------------------------------------------------------------------------

static void test_a_device_on_a_desk_reports_nothing() {
    // **The false-positive case, and it outranks the others.** The other five fire when a person is
    // holding the device and can see why. This one fires when nobody is there, and a character that
    // reacts to nothing is worse than one that misses a shake.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> still;
    for (uint32_t t = 0; t < 30000; t += 20) still.push_back(resting(t));

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(replay(detector, still).size()));
}

static void test_sensor_noise_does_not_become_a_motion() {
    // A real accelerometer at rest wobbles by a few thousandths of a g. Every threshold here has to
    // sit above that, and this is the test that says so.
    MotionDetector detector;
    detector.level(resting(0));

    // **`static_cast<int>` and not an accident.** The first version of this line wrote
    // `(t / 20) % 7 - 3` with `t` unsigned, so three samples in every seven underflowed to four
    // billion instead of going negative -- and the fixture fed the detector garbage rather than
    // noise. It reported 47 shakes, the test failed, and the defect was in the test.
    //
    // The same unsigned subtraction cost this project hours in v1.4, where a listening window
    // closed after 53 ms because a timestamp was subtracted from a smaller one. Writing it a
    // second time is why the note is here rather than just the cast.
    std::vector<AccelSample> noisy;
    for (uint32_t t = 0; t < 20000; t += 20) {
        const float wobble = static_cast<float>(static_cast<int>((t / 20) % 7) - 3) * 0.004f;
        noisy.push_back(AccelSample{wobble, -wobble, 1.0f + wobble, t});
    }

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(replay(detector, noisy).size()));
}

// ---------------------------------------------------------------------------------------
// The five motions
// ---------------------------------------------------------------------------------------

static void test_a_lean_is_a_tilt() {
    // Rotating the device tips gravity onto the x axis while its magnitude stays at 1 g.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> leaning;
    for (int step = 0; step <= 20; ++step) {
        const float fraction = static_cast<float>(step) / 20.0f;
        const float x = 0.5f * fraction;                 // up to 30 degrees
        const float z = 1.0f - 0.13f * fraction;         // what is left on the vertical
        leaning.push_back(AccelSample{x, 0.0f, z, static_cast<uint32_t>(1000 + step * 30)});
    }

    TEST_ASSERT_TRUE(contains(replay(detector, leaning), Motion::kTilt));
}

static void test_a_tilt_is_reported_once_not_continuously() {
    // Holding a device at an angle is one tilt. Reporting it every sample would send the server
    // fifty events a second for a device propped against a mug.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> held;
    for (uint32_t t = 1000; t < 6000; t += 20) held.push_back(AccelSample{0.5f, 0.0f, 0.86f, t});

    const auto out = replay(detector, held);
    int tilts = 0;
    for (const Motion motion : out) {
        if (motion == Motion::kTilt) ++tilts;
    }
    TEST_ASSERT_EQUAL_INT(1, tilts);
}

static void test_a_drop_is_free_fall() {
    // Nothing holding it up: the magnitude collapses. A real drop reads 0.1-0.3 g rather than a
    // clean zero, because the device rotates on the way down.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> dropped;
    for (uint32_t t = 1000; t < 1400; t += 20) {
        dropped.push_back(AccelSample{0.05f, 0.08f, 0.12f, t});
    }

    TEST_ASSERT_TRUE(contains(replay(detector, dropped), Motion::kFreeFall));
}

static void test_a_brief_jolt_is_not_a_drop() {
    // A sharp downward shove dips the magnitude for a moment. A desk is 70 cm, so a fall lasts
    // about 380 ms; anything far shorter is someone putting the device down hard.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> jolt;
    jolt.push_back(resting(1000));
    jolt.push_back(AccelSample{0.0f, 0.0f, 0.2f, 1020});
    jolt.push_back(AccelSample{0.0f, 0.0f, 0.2f, 1060});
    jolt.push_back(resting(1100));

    TEST_ASSERT_FALSE(contains(replay(detector, jolt), Motion::kFreeFall));
}

static void test_turning_it_over_is_upside_down() {
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> flipped;
    flipped.push_back(resting(1000));
    flipped.push_back(AccelSample{0.0f, 0.0f, -1.0f, 1400});

    TEST_ASSERT_TRUE(contains(replay(detector, flipped), Motion::kUpsideDown));
}

static void test_upside_down_is_reported_once_per_flip() {
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> held;
    held.push_back(resting(1000));
    for (uint32_t t = 1400; t < 4000; t += 20) held.push_back(AccelSample{0.0f, 0.0f, -1.0f, t});

    int flips = 0;
    for (const Motion motion : replay(detector, held)) {
        if (motion == Motion::kUpsideDown) ++flips;
    }
    TEST_ASSERT_EQUAL_INT(1, flips);
}

static void test_shaking_it_is_a_shake() {
    // The total swinging past twice gravity, three times inside 600 ms.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> shaken;
    uint32_t t = 1000;
    for (int swing = 0; swing < 4; ++swing) {
        shaken.push_back(AccelSample{2.2f, 0.0f, 1.0f, t});
        shaken.push_back(AccelSample{0.0f, 0.0f, 1.0f, t + 60});
        t += 120;
    }

    TEST_ASSERT_TRUE(contains(replay(detector, shaken), Motion::kShake));
}

static void test_one_knock_is_not_a_shake() {
    // A bump against the desk peaks near 1.3 g and happens once. Two would be "moved it somewhere".
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> knocked;
    knocked.push_back(resting(1000));
    knocked.push_back(AccelSample{1.9f, 0.0f, 1.0f, 1020});
    knocked.push_back(resting(1060));

    TEST_ASSERT_FALSE(contains(replay(detector, knocked), Motion::kShake));
}

static void test_two_knocks_far_apart_do_not_merge() {
    // **The window's other job.** Without it, two unrelated bumps a second apart would accumulate
    // into a shake nobody performed.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> knocks;
    for (uint32_t t = 1000; t < 6000; t += 1000) {
        knocks.push_back(AccelSample{2.2f, 0.0f, 1.0f, t});
        knocks.push_back(resting(t + 60));
    }

    TEST_ASSERT_FALSE(contains(replay(detector, knocks), Motion::kShake));
}

static void test_lifting_it_is_picked_up() {
    // Lifting takes the magnitude to 1.2-1.5 g on the way up, then back to 1 g at a new angle.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> lifted;
    lifted.push_back(resting(1000));
    for (uint32_t t = 1020; t < 1300; t += 20) lifted.push_back(AccelSample{0.1f, 0.1f, 1.45f, t});
    for (uint32_t t = 1300; t < 1800; t += 20) lifted.push_back(AccelSample{0.15f, 0.1f, 0.98f, t});

    TEST_ASSERT_TRUE(contains(replay(detector, lifted), Motion::kPickedUp));
}

// ---------------------------------------------------------------------------------------
// Hysteresis and priority
// ---------------------------------------------------------------------------------------

static void test_tilt_has_hysteresis() {
    // The property `lipsync.h` documents: a value sitting on a threshold flips on adjacent
    // samples, and a face reacting to that would twitch rather than respond.
    TEST_ASSERT_TRUE(roboface::kTiltReleaseG < roboface::kTiltG);
}

static void test_a_value_on_the_tilt_threshold_does_not_flutter() {
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> wobbling;
    for (uint32_t t = 1000; t < 5000; t += 20) {
        const float x = ((t / 20) % 2 == 0) ? 0.26f : 0.24f;
        wobbling.push_back(AccelSample{x, 0.0f, 0.96f, t});
    }

    int tilts = 0;
    for (const Motion motion : replay(detector, wobbling)) {
        if (motion == Motion::kTilt) ++tilts;
    }
    TEST_ASSERT_EQUAL_INT(1, tilts);
}

static void test_free_fall_outranks_everything() {
    // A falling device is also briefly shaken and tilted. Reporting all three would be true and
    // useless: only one of them matters in the next 200 ms.
    MotionDetector detector;
    detector.level(resting(0));

    std::vector<AccelSample> tumbling;
    for (uint32_t t = 1000; t < 1400; t += 20) {
        tumbling.push_back(AccelSample{0.2f, -0.15f, 0.1f, t});
    }

    const auto out = replay(detector, tumbling);
    TEST_ASSERT_TRUE(contains(out, Motion::kFreeFall));
    TEST_ASSERT_FALSE(contains(out, Motion::kTilt));
    TEST_ASSERT_FALSE(contains(out, Motion::kShake));
}

static void test_levelling_makes_a_shelf_not_a_tilt() {
    // A device on a shelf at an angle is not permanently tilted; it is a device on a shelf.
    MotionDetector detector;
    const AccelSample angled{0.45f, 0.0f, 0.89f, 0};
    detector.level(angled);

    std::vector<AccelSample> sitting;
    for (uint32_t t = 100; t < 5000; t += 20) sitting.push_back(AccelSample{0.45f, 0.0f, 0.89f, t});

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(replay(detector, sitting).size()));
}

static void test_magnitude_is_the_vector_length() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, roboface::magnitude(AccelSample{0.0f, 0.0f, 1.0f, 0}));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, roboface::magnitude(AccelSample{0.0f, 0.0f, 0.0f, 0}));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.414f, roboface::magnitude(AccelSample{1.0f, 0.0f, 1.0f, 0}));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_device_on_a_desk_reports_nothing);
    RUN_TEST(test_sensor_noise_does_not_become_a_motion);
    RUN_TEST(test_a_lean_is_a_tilt);
    RUN_TEST(test_a_tilt_is_reported_once_not_continuously);
    RUN_TEST(test_a_drop_is_free_fall);
    RUN_TEST(test_a_brief_jolt_is_not_a_drop);
    RUN_TEST(test_turning_it_over_is_upside_down);
    RUN_TEST(test_upside_down_is_reported_once_per_flip);
    RUN_TEST(test_shaking_it_is_a_shake);
    RUN_TEST(test_one_knock_is_not_a_shake);
    RUN_TEST(test_two_knocks_far_apart_do_not_merge);
    RUN_TEST(test_lifting_it_is_picked_up);
    RUN_TEST(test_tilt_has_hysteresis);
    RUN_TEST(test_a_value_on_the_tilt_threshold_does_not_flutter);
    RUN_TEST(test_free_fall_outranks_everything);
    RUN_TEST(test_levelling_makes_a_shelf_not_a_tilt);
    RUN_TEST(test_magnitude_is_the_vector_length);
    return UNITY_END();
}

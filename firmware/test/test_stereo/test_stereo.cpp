// v2.5 RF-073 — two channels, taken apart and measured.
//
// The whole point of `pure/stereo.h` is that a two-channel signal can be reasoned about without a
// board. These fixtures are the proof: a hard-panned source, a centred one, and silence, each with a
// number attached that a laptop can check.

#include <unity.h>

#include <cstdint>
#include <vector>

#include "pure/stereo.h"

using roboface::channelRms;
using roboface::deinterleave;
using roboface::measure;
using roboface::StereoLevels;

namespace {

//: A crude sine, integer-only, so the fixture has no dependency the header does not.
std::vector<int16_t> tone(std::size_t count, int16_t amplitude, int period = 40) {
    std::vector<int16_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        // A triangle rather than a sine: the shape does not matter to RMS, and this needs no
        // <cmath> -- the same constraint the header itself is under.
        const int phase = static_cast<int>(i % static_cast<std::size_t>(period));
        const int half = period / 2;
        const int ramp = phase < half ? phase : period - phase;
        out[i] = static_cast<int16_t>((2 * amplitude * ramp) / half - amplitude);
    }
    return out;
}

std::vector<int16_t> interleave(const std::vector<int16_t>& left,
                                const std::vector<int16_t>& right) {
    std::vector<int16_t> out(left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        out[2 * i] = left[i];
        out[2 * i + 1] = right[i];
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Deinterleaving
// ---------------------------------------------------------------------------------------

static void test_deinterleaving_is_exact() {
    const std::vector<int16_t> source = {1, -1, 2, -2, 3, -3, 4, -4};
    std::vector<int16_t> left(4), right(4);
    deinterleave(source.data(), 4, left.data(), right.data());

    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT16(static_cast<int16_t>(i + 1), left[i]);
        TEST_ASSERT_EQUAL_INT16(static_cast<int16_t>(-(i + 1)), right[i]);
    }
}

static void test_deinterleaving_an_empty_frame_is_not_a_crash() {
    std::vector<int16_t> left(1, 99), right(1, 99);
    deinterleave(nullptr, 0, left.data(), right.data());
    deinterleave(left.data(), 0, nullptr, right.data());
    // Nothing written, nothing dereferenced. The assertion is that we got here.
    TEST_ASSERT_EQUAL_INT16(99, left[0]);
}

static void test_a_stereo_frame_holds_the_same_duration_as_a_mono_one() {
    // **The arithmetic that would silently halve every timing in the capture path.** A stereo frame
    // has twice the samples for the same 20 ms; a version of this constant that kept the sample
    // count constant instead would keep 10 ms of sound and look entirely reasonable.
    TEST_ASSERT_EQUAL_UINT32(roboface::kCaptureFrameSamples * 2, roboface::kStereoFrameSamples);
    TEST_ASSERT_EQUAL_UINT32(320, roboface::kCaptureFrameSamples);  // 20 ms at 16 kHz
}

// ---------------------------------------------------------------------------------------
// Measuring
// ---------------------------------------------------------------------------------------

static void test_a_centred_source_is_balanced() {
    const auto signal = tone(320, 8000);
    const auto both = interleave(signal, signal);
    std::vector<int16_t> left(320), right(320);
    deinterleave(both.data(), 320, left.data(), right.data());

    const StereoLevels levels = measure(left.data(), right.data(), 320);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, levels.balance);
    TEST_ASSERT_TRUE(levels.left > 0.05f);
}

static void test_a_source_panned_left_reports_negative_balance() {
    const auto loud = tone(320, 12000);
    const auto quiet = tone(320, 1200);
    std::vector<int16_t> left(320), right(320);
    const auto both = interleave(loud, quiet);
    deinterleave(both.data(), 320, left.data(), right.data());

    const StereoLevels levels = measure(left.data(), right.data(), 320);

    TEST_ASSERT_TRUE(levels.balance < -0.5f);
    TEST_ASSERT_TRUE(levels.left > levels.right);
}

static void test_a_source_panned_right_reports_positive_balance() {
    const auto loud = tone(320, 12000);
    const auto quiet = tone(320, 1200);
    std::vector<int16_t> left(320), right(320);
    const auto both = interleave(quiet, loud);
    deinterleave(both.data(), 320, left.data(), right.data());

    const StereoLevels levels = measure(left.data(), right.data(), 320);

    TEST_ASSERT_TRUE(levels.balance > 0.5f);
}

static void test_silence_is_not_a_direction() {
    // **The case that would drive a face's gaze from nothing.** Two noise floors divided by each
    // other produce a confident-looking number; the floor in `measure` exists so that they do not.
    std::vector<int16_t> left(320, 0), right(320, 0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, measure(left.data(), right.data(), 320).balance);

    std::vector<int16_t> hiss_l(320), hiss_r(320);
    for (int i = 0; i < 320; ++i) {
        hiss_l[static_cast<std::size_t>(i)] = static_cast<int16_t>((i % 7) - 3);
        hiss_r[static_cast<std::size_t>(i)] = static_cast<int16_t>((i % 5) - 2);
    }
    const StereoLevels levels = measure(hiss_l.data(), hiss_r.data(), 320);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, levels.balance);
}

static void test_balance_does_not_depend_on_how_loud_the_source_is() {
    // The reason `balance` is a ratio and not a difference: a threshold on a difference means one
    // thing for a whisper and another for a shout, and only one of those was ever tested.
    std::vector<int16_t> quiet_l(320), quiet_r(320), loud_l(320), loud_r(320);
    const auto a = interleave(tone(320, 2000), tone(320, 1000));
    const auto b = interleave(tone(320, 20000), tone(320, 10000));
    deinterleave(a.data(), 320, quiet_l.data(), quiet_r.data());
    deinterleave(b.data(), 320, loud_l.data(), loud_r.data());

    const float quiet = measure(quiet_l.data(), quiet_r.data(), 320).balance;
    const float loud = measure(loud_l.data(), loud_r.data(), 320).balance;

    TEST_ASSERT_FLOAT_WITHIN(0.02f, quiet, loud);
}

static void test_rms_of_nothing_is_nothing() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, channelRms(nullptr, 320));
    std::vector<int16_t> zeros(320, 0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, channelRms(zeros.data(), 320));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, channelRms(zeros.data(), 0));
}

// ---------------------------------------------------------------------------------------
// Downmix — RF-076
// ---------------------------------------------------------------------------------------

static void test_two_identical_channels_survive_the_downmix_unchanged() {
    const auto signal = tone(320, 9000);
    std::vector<int16_t> out(320);
    roboface::downmix(signal.data(), signal.data(), 320, out.data());

    for (std::size_t i = 0; i < 320; ++i) {
        TEST_ASSERT_INT16_WITHIN(1, signal[i], out[i]);
    }
}

static void test_the_downmix_cannot_clip() {
    // **The reason it averages rather than sums.** Two correlated channels at full scale summed are
    // 6 dB over, and int16 would wrap -- turning the loudest half of every word into noise, audibly
    // and only for loud speakers, which is the worst possible way to find out.
    std::vector<int16_t> left(320, 32767), right(320, 32767);
    std::vector<int16_t> out(320);
    roboface::downmix(left.data(), right.data(), 320, out.data());
    for (std::size_t i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(32767, out[i]);

    std::vector<int16_t> min_l(320, -32768), min_r(320, -32768);
    roboface::downmix(min_l.data(), min_r.data(), 320, out.data());
    for (std::size_t i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(-32768, out[i]);
}

static void test_a_full_scale_channel_beside_a_silent_one_halves_rather_than_clipping() {
    std::vector<int16_t> loud(320, 30000), silent(320, 0), out(320);
    roboface::downmix(loud.data(), silent.data(), 320, out.data());
    for (std::size_t i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(15000, out[i]);
}

static void test_a_source_to_one_side_is_still_averaged() {
    // A voice off to the left is not a broken microphone, and mistaking one for the other would
    // throw away half the array on every off-centre speaker.
    std::vector<int16_t> l(320), r(320);
    const auto both = interleave(tone(320, 12000), tone(320, 6000));
    deinterleave(both.data(), 320, l.data(), r.data());

    TEST_ASSERT_EQUAL_INT(static_cast<int>(roboface::MonoSource::kBoth),
                          static_cast<int>(roboface::chooseSource(measure(l.data(), r.data(), 320))));
}

static void test_an_obstructed_channel_is_dropped_rather_than_averaged_in() {
    // A hand over one microphone. Averaging it in would halve the signal for no benefit.
    std::vector<int16_t> l(320), r(320);
    const auto both = interleave(tone(320, 12000), tone(320, 300));
    deinterleave(both.data(), 320, l.data(), r.data());

    const roboface::MonoSource source = roboface::chooseSource(measure(l.data(), r.data(), 320));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(roboface::MonoSource::kLeft), static_cast<int>(source));

    std::vector<int16_t> out(320);
    roboface::downmix(l.data(), r.data(), 320, out.data(), source);
    for (std::size_t i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(l[i], out[i]);
}

static void test_silence_does_not_pick_a_channel() {
    std::vector<int16_t> zeros(320, 0);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(roboface::MonoSource::kBoth),
        static_cast<int>(roboface::chooseSource(measure(zeros.data(), zeros.data(), 320))));
}

static void test_the_uplink_frame_is_the_same_size_it_always_was() {
    // Part of the v2.5 DoD, asserted rather than assumed: the stereo work is the device's and the
    // server sees exactly the stream it always did.
    TEST_ASSERT_EQUAL_UINT32(640, roboface::kCaptureFrameBytes);
    TEST_ASSERT_EQUAL_UINT32(roboface::kCaptureFrameSamples * 2, roboface::kCaptureFrameBytes);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_deinterleaving_is_exact);
    RUN_TEST(test_deinterleaving_an_empty_frame_is_not_a_crash);
    RUN_TEST(test_a_stereo_frame_holds_the_same_duration_as_a_mono_one);
    RUN_TEST(test_a_centred_source_is_balanced);
    RUN_TEST(test_a_source_panned_left_reports_negative_balance);
    RUN_TEST(test_a_source_panned_right_reports_positive_balance);
    RUN_TEST(test_silence_is_not_a_direction);
    RUN_TEST(test_balance_does_not_depend_on_how_loud_the_source_is);
    RUN_TEST(test_rms_of_nothing_is_nothing);
    RUN_TEST(test_two_identical_channels_survive_the_downmix_unchanged);
    RUN_TEST(test_the_downmix_cannot_clip);
    RUN_TEST(test_a_full_scale_channel_beside_a_silent_one_halves_rather_than_clipping);
    RUN_TEST(test_a_source_to_one_side_is_still_averaged);
    RUN_TEST(test_an_obstructed_channel_is_dropped_rather_than_averaged_in);
    RUN_TEST(test_silence_does_not_pick_a_channel);
    RUN_TEST(test_the_uplink_frame_is_the_same_size_it_always_was);
    return UNITY_END();
}

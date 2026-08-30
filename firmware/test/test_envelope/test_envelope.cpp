// The envelope the mouth is driven by, against actual samples.
//
// **Fixtures are generated here, not stored.** A binary blob of PCM in the repository cannot be read
// by the next person, cannot be adjusted, and cannot explain what it is meant to represent. These
// are built from parameters — a tone at a stated amplitude, a burst train with stated gaps — so the
// thing being asserted is visible in the same screen as the assertion.
//
// The property that matters is not "it returns a number in range". It is that the envelope **moves
// with speech**, which is exactly what the peak detector it replaces failed at: on the board a peak
// over one 32 ms chunk sat near full scale for almost all of continuous speech, and the first viseme
// ladder therefore changed shape four times in ten seconds. A test that only checked bounds would
// have passed against that.

#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pure/envelope.h"

using roboface::followEnvelope;
using roboface::rmsLevel;

namespace {

//: One playback chunk: 512 samples, 32 ms at 16 kHz. The unit the audio path hands over, so the
//: tests measure exactly what the device measures.
constexpr std::size_t kChunk = 512;

//: A sine at a given amplitude, in samples. `amplitude` is 0..1 of full scale.
std::vector<int16_t> tone(std::size_t count, double amplitude, double cycles = 8.0) {
    std::vector<int16_t> out(count);
    // A hand-rolled sine so the fixture does not depend on <cmath>'s precision differing between
    // the host and the target -- and because the shape matters, not the exact spectrum.
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 6.283185307179586 * cycles * static_cast<double>(i) /
                             static_cast<double>(count);
        // Taylor-free: iterate a rotation instead. Simpler here to approximate with a triangle
        // folded into a smooth-enough curve -- what is being tested is energy, not harmonics.
        double x = phase;
        while (x > 3.141592653589793) x -= 6.283185307179586;
        const double s = x * (1.0 - (x * x) / 6.0 + (x * x * x * x) / 120.0);
        const double clamped = s > 1.0 ? 1.0 : (s < -1.0 ? -1.0 : s);
        out[i] = static_cast<int16_t>(clamped * amplitude * 32767.0);
    }
    return out;
}

std::vector<int16_t> silence(std::size_t count) { return std::vector<int16_t>(count, 0); }

//: Full scale, hard-clipped -- the shape a loud passage actually has after a limiter.
std::vector<int16_t> clipped(std::size_t count) {
    std::vector<int16_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = (i % 32 < 16) ? 32767 : -32768;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The measure itself
// ---------------------------------------------------------------------------------------

static void test_silence_reads_as_silence() {
    const auto samples = silence(kChunk);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rmsLevel(samples.data(), samples.size()));
}

static void test_an_empty_block_is_zero_rather_than_a_division() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rmsLevel(nullptr, 0));
    const auto samples = tone(kChunk, 1.0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rmsLevel(samples.data(), 0));
}

static void test_a_constant_full_scale_block_is_one() {
    // The degenerate case that pins the scaling: every sample at full scale means RMS is full scale.
    std::vector<int16_t> flat(kChunk, 32767);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, rmsLevel(flat.data(), flat.size()));
}

static void test_a_clipped_signal_never_exceeds_one() {
    // A limiter's output is the loudest thing the speaker ever plays. If this can exceed 1.0 the
    // viseme ladder's top band is unreachable in exactly the passages that should reach it.
    const auto samples = clipped(kChunk);
    const float level = rmsLevel(samples.data(), samples.size());
    TEST_ASSERT_TRUE(level <= 1.0f);
    TEST_ASSERT_TRUE(level > 0.9f);
}

static void test_rms_scales_with_amplitude() {
    const auto loud = tone(kChunk, 1.0);
    const auto half = tone(kChunk, 0.5);
    const auto quiet = tone(kChunk, 0.1);

    const float a = rmsLevel(loud.data(), loud.size());
    const float b = rmsLevel(half.data(), half.size());
    const float c = rmsLevel(quiet.data(), quiet.size());

    TEST_ASSERT_TRUE(a > b);
    TEST_ASSERT_TRUE(b > c);
    // Halving the amplitude halves the RMS -- the linearity a threshold ladder depends on. A
    // measure that compressed here would make the bands uneven for reasons no tuning could fix.
    TEST_ASSERT_FLOAT_WITHIN(0.03f, a / 2.0f, b);
}

static void test_a_large_block_does_not_overflow_the_accumulator() {
    // 512 samples squared at full scale is ~5.5e11 -- two hundred times a 32-bit accumulator. The
    // passages that would overflow are the loudest ones, which is the worst place for a wrap.
    std::vector<int16_t> flat(4096, -32768);
    const float level = rmsLevel(flat.data(), flat.size());
    TEST_ASSERT_TRUE(level > 0.9f && level <= 1.0f);
}

// ---------------------------------------------------------------------------------------
// The property the peak detector failed
// ---------------------------------------------------------------------------------------

static void test_the_envelope_moves_over_a_speech_like_signal() {
    // **The reason this module exists.** A burst train -- loud syllable, short gap, repeated -- is
    // what speech looks like at this timescale. The envelope has to span a wide range over it. The
    // peak detector this replaces produced a near-constant number for the same input, which is why
    // the mouth held one shape for whole replies.
    float low = 1.0f;
    float high = 0.0f;

    for (int syllable = 0; syllable < 12; ++syllable) {
        const auto loud = tone(kChunk, 0.8);
        const auto gap = tone(kChunk, 0.02);
        for (const auto* block : {&loud, &gap}) {
            const float level = rmsLevel(block->data(), block->size());
            if (level < low) low = level;
            if (level > high) high = level;
        }
    }

    // The span is the assertion: a measure that saturates would give high ≈ low ≈ 1.
    TEST_ASSERT_TRUE(high > 0.4f);
    TEST_ASSERT_TRUE(low < 0.1f);
    TEST_ASSERT_TRUE(high - low > 0.4f);
}

// ---------------------------------------------------------------------------------------
// Smoothing
// ---------------------------------------------------------------------------------------

static void test_a_rise_is_immediate() {
    // A consonant has to land on the frame it arrives in, or the mouth is visibly late.
    TEST_ASSERT_EQUAL_FLOAT(0.9f, followEnvelope(0.1f, 0.9f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, followEnvelope(0.0f, 1.0f));
}

static void test_a_fall_is_eased() {
    const float after = followEnvelope(1.0f, 0.0f);
    TEST_ASSERT_TRUE(after > 0.0f);
    TEST_ASSERT_TRUE(after < 1.0f);
}

static void test_the_fall_takes_longer_than_a_syllable_gap() {
    // Roughly 160 ms to a tenth -- longer than the gap between syllables, shorter than the pause
    // between words. A mouth that shut in every syllable gap would read as chattering.
    float level = 1.0f;
    int chunks = 0;
    while (level > 0.1f && chunks < 100) {
        level = followEnvelope(level, 0.0f);
        ++chunks;
    }
    // 32 ms per chunk: 4-7 chunks is 128-224 ms.
    TEST_ASSERT_TRUE(chunks >= 4);
    TEST_ASSERT_TRUE(chunks <= 7);
}

static void test_smoothing_never_leaves_the_unit_range() {
    float level = 0.0f;
    for (int i = 0; i < 200; ++i) {
        level = followEnvelope(level, (i % 3 == 0) ? 1.0f : 0.0f);
        TEST_ASSERT_TRUE(level >= 0.0f && level <= 1.0f);
    }
}

static void test_smoothing_preserves_the_span_of_a_burst_train() {
    // Smoothing that flattened the signal would undo what the RMS bought. Fed the same burst train,
    // the smoothed envelope must still span a usable range.
    float level = 0.0f;
    float low = 1.0f;
    float high = 0.0f;

    // **A syllable is not one chunk.** Speech runs at four to seven syllables a second, so a
    // syllable is 150-250 ms -- five to eight of these 32 ms chunks -- and the gap between two is
    // shorter still. The first version of this fixture alternated every single chunk, which is a
    // signal four times faster than anything a mouth has to follow, and the smoothing correctly
    // flattened it. The test was wrong, not the filter; a fixture that does not model speech
    // cannot be used to tune something for speech.
    for (int syllable = 0; syllable < 12; ++syllable) {
        const auto loud = tone(kChunk, 0.8);
        const auto gap = tone(kChunk, 0.02);
        for (int chunk = 0; chunk < 5; ++chunk) {
            level = followEnvelope(level, rmsLevel(loud.data(), loud.size()));
            if (syllable >= 2) { if (level < low) low = level; if (level > high) high = level; }
        }
        for (int chunk = 0; chunk < 2; ++chunk) {
            level = followEnvelope(level, rmsLevel(gap.data(), gap.size()));
            if (syllable >= 2) { if (level < low) low = level; if (level > high) high = level; }
        }
    }

    TEST_ASSERT_TRUE(high - low > 0.3f);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_silence_reads_as_silence);
    RUN_TEST(test_an_empty_block_is_zero_rather_than_a_division);
    RUN_TEST(test_a_constant_full_scale_block_is_one);
    RUN_TEST(test_a_clipped_signal_never_exceeds_one);
    RUN_TEST(test_rms_scales_with_amplitude);
    RUN_TEST(test_a_large_block_does_not_overflow_the_accumulator);
    RUN_TEST(test_the_envelope_moves_over_a_speech_like_signal);
    RUN_TEST(test_a_rise_is_immediate);
    RUN_TEST(test_a_fall_is_eased);
    RUN_TEST(test_the_fall_takes_longer_than_a_syllable_gap);
    RUN_TEST(test_smoothing_never_leaves_the_unit_range);
    RUN_TEST(test_smoothing_preserves_the_span_of_a_burst_train);
    return UNITY_END();
}

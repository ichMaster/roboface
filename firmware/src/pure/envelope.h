// The envelope the mouth is driven by.
//
// **Separate from `level.h`, and the split is the point.** That header measures a *peak* and says
// why: a level meter is watched by a person checking whether the device is hearing them right now,
// so the loudest sample is the honest number and RMS would lag into looking like a bug.
//
// A mouth is not a meter. It is modelling how far a jaw is open, which is a question about energy
// over a window rather than about the single loudest sample in it — and the difference is not
// academic. Measured on the board: a peak over one 32 ms playback chunk of continuous speech sits
// near full scale almost the entire time. `lvl=0..93%` per ten-second window, with the first
// version of the viseme ladder changing shape **four times in ten seconds**, because the signal it
// was reading barely moved.
//
// The workaround then was to spread the thresholds across the observed range, which worked and
// treated the symptom: it compensated for a signal whose shape was wrong. This is the shape being
// right instead — and roadmap §v2.3 asked for it in the first place.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>, no clock. Arithmetic over samples,
// so a laptop can prove what a mouth on a desk cannot be asked.

#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

//: How much of the previous envelope survives a *fall*. Rises are immediate.
//:
//: **The asymmetry is the whole design.** Speech has hard onsets and soft decays: a consonant has
//: to land on the frame it arrives in, or the mouth is visibly late. Falling at the same speed
//: would slam the mouth shut in the gap between every syllable — the gaps are short, and a mouth
//: that closes in all of them reads as chattering rather than as talking.
//:
//: 0.55 over 32 ms chunks means a fall reaches a tenth of its starting value in about five chunks,
//: which is 160 ms — longer than a syllable gap and shorter than a pause between words.
inline constexpr float kEnvelopeFall = 0.55f;

// Root-mean-square of a block of samples, as 0..1.
//
// The measure roadmap §v2.3 specifies. Computed over the block the audio path already has in hand
// on its way to the speaker — 512 samples, 32 ms at 16 kHz — so nothing is buffered for it and the
// I2S path is not touched.
//
// Accumulated in `int64_t`: 512 samples squared is at most 512 × 32768² ≈ 5.5e11, which overflows
// a 32-bit accumulator two hundred times over. On a device whose loudest passages are exactly the
// ones that would overflow, that is not a theoretical concern.
inline float rmsLevel(const int16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) return 0.0f;

    int64_t sum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int64_t value = samples[i];
        sum += value * value;
    }

    // **Exactly zero, not nearly zero.** Newton's method below halves its way toward the root and
    // never arrives: from a seed of 1.0 it reaches 9.5e-7 after twenty iterations, which scales to
    // 2.9e-11 -- a number that is not silence, sits above no threshold, and would have been
    // invisible until someone compared an envelope against zero. Found by the test that does.
    if (sum == 0) return 0.0f;

    // Integer mean first, then one square root — rather than a running float mean, which would lose
    // precision over a long block for no benefit.
    const double mean = static_cast<double>(sum) / static_cast<double>(count);

    // Newton's method rather than <cmath>: this header is compiled for both the host and an
    // ESP32-S3, and `sqrt` on a float is a call the compiler is free to make expensive. Five
    // iterations from a mean-based seed converge to well past the precision a mouth needs.
    double root = mean > 1.0 ? mean : 1.0;
    for (int i = 0; i < 20; ++i) {
        const double next = 0.5 * (root + mean / root);
        if (next == root) break;
        root = next;
    }

    const float level = static_cast<float>(root / 32767.0);
    return level > 1.0f ? 1.0f : level;
}

// Follow a rise at once, ease a fall.
//
// The same shape as `decayToward` in `level.h` and deliberately not the same constant: a meter and a
// mouth ease at different speeds because they are answering different questions. Written here rather
// than reused so that changing one cannot silently change the other — they have been tuned apart
// once already.
inline float followEnvelope(float previous, float next, float fall = kEnvelopeFall) {
    if (next >= previous) return next;
    return previous * fall + next * (1.0f - fall);
}

}  // namespace roboface

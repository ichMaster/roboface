// Two channels: taking them apart, measuring them, and putting them back together.
//
// The Core S3 has two microphones on an ES7210, and until v2.5 the device used one of them. This
// header is the whole of what the second one is for, expressed as arithmetic rather than as an audio
// pipeline: **deinterleave, measure, combine**. Nothing here knows about I2S, DMA, or M5Unified.
//
// **Interleaved is how the hardware delivers it** -- `L R L R ...` in one buffer -- and interleaved
// is a terrible shape for every question worth asking. So the first thing that happens to a stereo
// frame is that it stops being one.
//
// Pure: header-only, `namespace roboface`, no <M5Unified.h>. A two-channel recording made on a desk
// can be replayed here on a laptop, which is the only way the direction thresholds in `direction.h`
// were ever going to be testable.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/capture.h"

namespace roboface {

//: How many channels the microphone delivers. Named, because the alternative is a `2` appearing in
//: the DMA size, the deinterleave loop and the frame arithmetic, where two of the three can be
//: changed together and the third found later by someone holding an oscilloscope.
inline constexpr std::size_t kCaptureChannels = 2;

//: One interleaved stereo frame holds this many int16 samples -- twice a mono frame's, for the same
//: 20 ms of sound. The duration is what stays constant, and that is worth stating: a stereo frame
//: that held 20 ms of *samples* would hold 10 ms of *time*, and every timing in the capture path
//: would silently halve.
inline constexpr std::size_t kStereoFrameSamples = kCaptureFrameSamples * kCaptureChannels;

//: Split `L R L R ...` into two contiguous runs.
//:
//: `count` is the number of **frames** (sample pairs), not of int16s -- the units that have caused
//: every off-by-two in this kind of code. Writing it as frames means the caller cannot pass the
//: interleaved length by accident and get half a signal that still looks plausible.
inline void deinterleave(const int16_t* interleaved, std::size_t frames, int16_t* left,
                         int16_t* right) {
    if (interleaved == nullptr || left == nullptr || right == nullptr) return;
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = interleaved[2 * i];
        right[i] = interleaved[2 * i + 1];
    }
}

//: RMS of one channel, in the same 0..1 scale `envelope.h` uses -- and by the same method, for the
//: same reason: no <cmath>, so this header compiles identically for the host and the ESP32-S3, and a
//: threshold tested on a laptop is the threshold that runs on the board.
inline float channelRms(const int16_t* samples, std::size_t count) {
    if (samples == nullptr || count == 0) return 0.0f;
    int64_t sum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int64_t value = samples[i];
        sum += value * value;
    }
    if (sum == 0) return 0.0f;  // Newton never arrives at zero; see envelope.h

    const float mean = static_cast<float>(sum) / static_cast<float>(count);
    float root = mean > 1.0f ? mean : 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float next = 0.5f * (root + mean / root);
        if (next == root) break;
        root = next;
    }
    return root / 32768.0f;
}

//: What the two channels measured, and how far apart they are.
struct StereoLevels {
    float left = 0.0f;
    float right = 0.0f;

    //: `(right - left) / (right + left)`, in [-1, +1]: negative is louder on the left.
    //:
    //: **A ratio, not a difference in dB.** A difference in level scales with how loud the source
    //: is, so a threshold on it would mean one thing for a whisper and another for a shout. This is
    //: normalised by construction, which is what lets `direction.h` state a threshold once.
    //:
    //: Zero when both channels are silent -- "no sound" is not "sound from dead centre", and the two
    //: must not produce the same number.
    float balance = 0.0f;
};

inline StereoLevels measure(const int16_t* left, const int16_t* right, std::size_t count) {
    StereoLevels levels;
    levels.left = channelRms(left, count);
    levels.right = channelRms(right, count);

    const float total = levels.left + levels.right;
    //: A floor rather than a test against zero. Two channels of near-silence produce a ratio of two
    //: noise floors, which is a confident-looking number about nothing at all -- and that number
    //: would drive a face's gaze.
    constexpr float kSilenceFloor = 0.0015f;
    if (total > kSilenceFloor) {
        levels.balance = (levels.right - levels.left) / total;
    }
    return levels;
}

//: How much quieter one channel must be than the other before it is treated as obstructed rather
//: than merely further away.
//:
//: A hand, a case or a desk edge over one microphone drops it far below the other -- a factor of
//: five or more. A source genuinely to one side gives a ratio well under two, which is the whole
//: basis of `balance` and must not be mistaken for a fault. 0.20 sits between those, closer to the
//: obstruction end, because averaging in a merely-quieter channel costs nothing while averaging in
//: a covered one costs half the signal.
inline constexpr float kObstructedChannelRatio = 0.20f;

//: Which channel or channels the uplink should be made from.
enum class MonoSource : uint8_t {
    kBoth,   // averaged
    kLeft,   // the right one is obstructed
    kRight,  // the left one is obstructed
};

//: Decide before mixing. Separated from the mixing itself so the decision can be tested against
//: levels alone, and so the reason a frame was built from one microphone is inspectable.
inline MonoSource chooseSource(const StereoLevels& levels) {
    const float louder = levels.left > levels.right ? levels.left : levels.right;
    if (louder <= 0.0f) return MonoSource::kBoth;

    const float quieter = levels.left > levels.right ? levels.right : levels.left;
    if (quieter / louder >= kObstructedChannelRatio) return MonoSource::kBoth;
    return levels.left > levels.right ? MonoSource::kLeft : MonoSource::kRight;
}

//: Two channels into the one the uplink carries.
//:
//: **Averaged, not summed.** Summing two correlated channels adds 6 dB and clips a loud speaker
//: into a square wave -- and the two channels here are correlated almost by construction, being
//: 40 mm apart. Averaging cannot clip, and the 3 dB it "loses" against a sum is not a loss at all:
//: it is the sum's gain, which was never wanted.
//:
//: The noise argument is the actual reason to combine rather than to pick: averaging two channels
//: of the same signal keeps the signal and halves the uncorrelated noise between them, which is
//: 3 dB of free signal-to-noise. That is the whole benefit of the second microphone to ASR.
inline void downmix(const int16_t* left, const int16_t* right, std::size_t count, int16_t* out,
                    MonoSource source = MonoSource::kBoth) {
    if (out == nullptr) return;
    if (left == nullptr || right == nullptr) return;

    switch (source) {
        case MonoSource::kLeft:
            for (std::size_t i = 0; i < count; ++i) out[i] = left[i];
            return;
        case MonoSource::kRight:
            for (std::size_t i = 0; i < count; ++i) out[i] = right[i];
            return;
        case MonoSource::kBoth:
            break;
    }

    for (std::size_t i = 0; i < count; ++i) {
        // In int32 before the halving. Two int16 values summed overflow int16 by exactly one bit,
        // and the compiler would be within its rights to wrap -- turning the loudest half of every
        // word into noise, audibly and only for loud speakers.
        const int32_t sum = static_cast<int32_t>(left[i]) + static_cast<int32_t>(right[i]);
        out[i] = static_cast<int16_t>(sum / 2);
    }
}

}  // namespace roboface

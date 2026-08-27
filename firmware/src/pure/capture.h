// The capture side's arithmetic: how a stream of samples becomes frames on the wire.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>. Small, but the numbers here decide
// whether v1.3's recognition sees speech as it happens or in one lump at the end, and getting the
// frame size wrong is not a crash -- it is a latency figure nobody notices until the whole loop
// feels slow.
//
// **20 ms per frame.** ROADMAP §v1.2 asks for 20-40 ms. The lower end is chosen because the frame
// interval is a floor on how late the server can learn anything: a 40 ms frame halves the syscalls
// and doubles the granularity of "audio arrives while you are still speaking", which is the
// property the phase exists to deliver.

#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

//: The device's capture format, and the server's: `AUDIO_FMT` in `protocol.py` is
//: `pcm16/16000/1`. One channel for now -- v2.5 adds the second microphone for direction.
inline constexpr int kCaptureSampleRate = 16000;
inline constexpr int kCaptureBytesPerSample = 2;
inline constexpr int kCaptureFrameMs = 20;

//: How many samples make one frame at the target interval, and how many bytes that is.
inline constexpr std::size_t kCaptureFrameSamples =
    static_cast<std::size_t>(kCaptureSampleRate) * kCaptureFrameMs / 1000;
inline constexpr std::size_t kCaptureFrameBytes =
    kCaptureFrameSamples * static_cast<std::size_t>(kCaptureBytesPerSample);

// Samples in `ms` at `rate`. Truncating rather than rounding: a frame that claimed more samples
// than it holds would read past the buffer, and a frame slightly short is merely a frame slightly
// short.
inline constexpr std::size_t samplesForMs(int ms, int rate = kCaptureSampleRate) {
    if (ms <= 0 || rate <= 0) return 0;
    return static_cast<std::size_t>(rate) * static_cast<std::size_t>(ms) / 1000;
}

// How long `bytes` of PCM16 lasts, in milliseconds. Used to report an utterance's length from what
// arrived rather than from a clock -- the same reason the server caps in bytes.
inline constexpr uint32_t msForBytes(std::size_t bytes, int rate = kCaptureSampleRate) {
    if (rate <= 0) return 0;
    const std::size_t samples = bytes / static_cast<std::size_t>(kCaptureBytesPerSample);
    return static_cast<uint32_t>(samples * 1000u / static_cast<std::size_t>(rate));
}

// What one utterance sent, so the device can state it rather than estimate it.
//
// The DoD is "frames arrive *before* `listen_stop`", and the evidence for that on the device side
// is a count that is already non-zero when the window closes. A tally that only existed on the
// server would make the device's half of that claim unverifiable from the board.
class CaptureTally {
  public:
    void reset() {
        frames_ = 0;
        bytes_ = 0;
    }

    void recordFrame(std::size_t bytes) {
        ++frames_;
        bytes_ += bytes;
    }

    uint32_t frames() const { return frames_; }
    std::size_t bytes() const { return bytes_; }
    uint32_t durationMs() const { return msForBytes(bytes_); }
    bool empty() const { return frames_ == 0; }

  private:
    uint32_t frames_ = 0;
    std::size_t bytes_ = 0;
};

}  // namespace roboface

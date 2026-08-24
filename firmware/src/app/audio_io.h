// Streaming playback: PCM arriving over the socket comes out of the speaker as it arrives.
//
// Glue, because it owns the hardware. The arithmetic that decides whether a byte is lost or a word
// is clipped lives in `pure/ring_buffer.h`, where a host test proves it.
//
// **The Core S3 shares one I2S bus between the microphone and the speaker**, so claiming it is not
// free and must not happen per frame. `startSpeaking()` switches once, on the first chunk of a
// turn; `finish()` drains and gives it back. A turn that took the bus and never returned it would
// leave the device unable to listen -- and the symptom appears one turn later, in v1.2's capture
// path, which is working correctly.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/ring_buffer.h"

namespace app {

//: About half a second of 16 kHz PCM16. Large enough that a slow frame does not starve playback,
//: small enough to leave PSRAM for the face's sprite -- and the ring buffer reports backpressure
//: rather than dropping, so a bigger buffer buys smoothness, never correctness.
inline constexpr std::size_t kPlaybackBufferBytes = 16384;

class AudioIo {
  public:
    // Sets the volume and prepares the speaker without claiming the bus.
    bool begin(uint8_t volume);

    // Take the bus for playback. Idempotent: the second chunk of a turn must not re-switch.
    void startSpeaking();

    // Buffer PCM16. Returns how many bytes were accepted -- a short return is backpressure, and
    // the caller offers the rest again after `tick()` has drained some.
    std::size_t write(const uint8_t* data, std::size_t length);

    // Move buffered audio to the speaker. Call every loop.
    void tick();

    // `tts_end`: play what is left, then give the bus back.
    void finish();

    // `restart` or an error: stop now, discard what is buffered, give the bus back. Speech that
    // has been superseded is worse than silence -- it answers a question nobody is still asking.
    void abort();

    bool isSpeaking() const { return speaking_; }
    bool isDraining() const { return draining_; }
    std::size_t buffered() const { return buffer_.size(); }
    uint32_t bytesPlayed() const { return bytes_played_; }

  private:
    void releaseBus();

    roboface::RingBuffer<kPlaybackBufferBytes> buffer_;
    bool speaking_ = false;
    bool draining_ = false;
    uint8_t volume_ = 128;
    uint32_t bytes_played_ = 0;
};

}  // namespace app

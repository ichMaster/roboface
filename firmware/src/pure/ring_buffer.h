// The playback buffer: bytes in from the socket, bytes out to the speaker.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>. The two properties that matter
// are arithmetic, and both are the kind that produce audible faults rather than crashes -- so they
// are proved on a laptop rather than discovered by listening.
//
// **Lossless.** `write` reports how many bytes it accepted and `read` how many it gave. Neither
// silently discards: a buffer that dropped the tail of a chunk when it was nearly full would lose
// a few milliseconds of speech at exactly the moments the network was struggling, and the symptom
// -- occasional clipped words under load -- is indistinguishable from a bad voice model.
//
// **Underrun is a pause, not silence.** A starved `read` returns fewer bytes than asked for,
// including zero, and the caller waits. Filling the gap with zeros would emit silence into the
// middle of a word, which does not sound like a stall -- it sounds like the sentence ended.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace roboface {

template <std::size_t Capacity>
class RingBuffer {
  public:
    static constexpr std::size_t capacity() { return Capacity; }

    std::size_t size() const { return size_; }
    std::size_t free() const { return Capacity - size_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == Capacity; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

    // Accepts as much as fits and returns how much that was. A short return is backpressure, not
    // an error: the caller holds the rest and offers it again once the speaker has drained some.
    std::size_t write(const uint8_t* data, std::size_t length) {
        const std::size_t accepted = length < free() ? length : free();
        for (std::size_t i = 0; i < accepted; ++i) {
            buffer_[tail_] = data[i];
            tail_ = (tail_ + 1) % Capacity;
        }
        size_ += accepted;
        return accepted;
    }

    // Fills up to `length` bytes and returns how many. Zero means starved, which the caller must
    // treat as "wait", never as "play zeros".
    std::size_t read(uint8_t* out, std::size_t length) {
        const std::size_t given = length < size_ ? length : size_;
        for (std::size_t i = 0; i < given; ++i) {
            out[i] = buffer_[head_];
            head_ = (head_ + 1) % Capacity;
        }
        size_ -= given;
        return given;
    }

    // Reads only whole PCM16 samples. Handing the speaker an odd byte count would shift every
    // following sample by a byte, which turns the rest of the phrase to noise rather than
    // producing one click.
    std::size_t readSamples(uint8_t* out, std::size_t length) {
        const std::size_t whole = (length < size_ ? length : size_) & ~static_cast<std::size_t>(1);
        return read(out, whole);
    }

  private:
    uint8_t buffer_[Capacity] = {};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
};

}  // namespace roboface

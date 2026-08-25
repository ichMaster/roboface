// The playback backlog: PCM in from the socket, PCM out to the speaker, in order and complete.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>. A view over memory the caller
// owns, because the buffer this needs is far too large for internal RAM -- a fifteen-second reply
// is about 480 KB of 16 kHz PCM16 -- so the glue allocates it in PSRAM and hands it over.
//
// This exists because `M5.Speaker.playRaw` **does not wait for a free slot: it returns false and
// drops the chunk** (`Speaker_Class::_set_next_wav` returns false when both slots on the channel
// are claimed). The network delivers a reply much faster than 16 kHz real time, so slots fill
// within the first second and everything after that is discarded. The audible symptom is exact:
// the beginning plays cleanly and then the sound breaks up.
//
// So the backlog is held here and fed to the speaker only as fast as it accepts.

#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

class PcmRing {
  public:
    // `storage` must outlive this object and hold at least `capacity` bytes.
    void attach(uint8_t* storage, std::size_t capacity) {
        storage_ = storage;
        capacity_ = capacity;
        clear();
    }

    bool attached() const { return storage_ != nullptr && capacity_ > 0; }
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return size_; }
    std::size_t free() const { return capacity_ - size_; }
    bool empty() const { return size_ == 0; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

    // Accepts as much as fits and reports it. A short return is backpressure the caller must
    // honour -- dropping the remainder loses speech, and the gap sounds like a bad voice rather
    // than a full buffer.
    std::size_t write(const uint8_t* data, std::size_t length) {
        const std::size_t accepted = length < free() ? length : free();
        for (std::size_t i = 0; i < accepted; ++i) {
            storage_[tail_] = data[i];
            tail_ = tail_ + 1 == capacity_ ? 0 : tail_ + 1;
        }
        size_ += accepted;
        return accepted;
    }

    // Copies out whole PCM16 samples only. An odd byte count handed to the speaker shifts every
    // following sample by a byte, which turns the rest of the phrase to noise rather than
    // producing one click.
    std::size_t readSamples(uint8_t* out, std::size_t length) {
        std::size_t give = length < size_ ? length : size_;
        give &= ~static_cast<std::size_t>(1);
        for (std::size_t i = 0; i < give; ++i) {
            out[i] = storage_[head_];
            head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
        }
        size_ -= give;
        return give;
    }

    // Put back the bytes just read, at the **front**. The speaker refuses a buffer when both its
    // slots are claimed, and the chunk must stay next in line: appending it to the tail instead
    // would reorder the reply, which is worse than the pause it was trying to avoid.
    void unread(std::size_t length) {
        const std::size_t give = length < capacity_ - size_ ? length : capacity_ - size_;
        for (std::size_t i = 0; i < give; ++i) {
            head_ = head_ == 0 ? capacity_ - 1 : head_ - 1;
        }
        size_ += give;
    }

  private:
    uint8_t* storage_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
};

}  // namespace roboface

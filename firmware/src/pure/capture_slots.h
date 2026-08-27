#pragma once

#include <cstddef>

namespace roboface {

//: Which capture buffer the loop may read, and which one it arms in exchange.
//
// Three buffers, and the third one is the entire reason this type exists. The microphone queue is
// two deep, so keeping the recorder busy while a completed frame is drained means the frame being
// drained cannot be one of the two the recorder owns. With only two buffers there is no such slot,
// and the natural code -- re-arm the buffer that just completed, then send it -- hands a buffer
// back to the DMA while its contents are still being read. Every frame then leaves as a blend of
// two moments in time.
//
// Nothing reports that. The frame count is right, the byte count is right, the level meter moves
// and the peak looks plausible; only the audio is wrong, and the first thing downstream that can
// tell is a recogniser returning an empty string for a room that was being spoken in.
class CaptureSlots {
public:
    static constexpr std::size_t kCount = 3;

    //: Buffers 0 and 1 are armed by the caller; 2 is the spare.
    void reset() {
        read_ = 0;
        spare_ = 2;
    }

    //: The buffer the recorder finishes next -- buffers complete in the order they were armed.
    std::size_t readSlot() const { return read_; }

    //: The buffer to arm now: never `readSlot()`, so the recorder stays two deep without ever
    //: being handed the frame that is still being read.
    std::size_t spareSlot() const { return spare_; }

    //: The frame at `readSlot()` has been consumed; it becomes the spare and the rotation advances.
    void advance() {
        const std::size_t consumed = read_;
        read_ = (consumed + 1) % kCount;
        spare_ = consumed;
    }

private:
    std::size_t read_ = 0;
    std::size_t spare_ = 2;
};

}  // namespace roboface

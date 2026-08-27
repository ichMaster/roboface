// Which slot the next pre-roll frame goes in, and which order they come back out.
//
// **Pure**: the ring's *storage* is glue -- it owns buffers and lives next to the recorder -- but
// where a frame goes and what order they leave in is arithmetic, and arithmetic belongs where a
// host can check it. The bug this prevents is not dropping frames; it is replaying them in the
// wrong order, which produces audio that sounds almost right and recognises as nothing.
//
// The ring keeps the **last** N frames and discards the oldest, because pre-roll is only ever
// interested in what was just said.

#pragma once

#include <cstddef>

namespace roboface {

class PreRollRing {
  public:
    //: `capacity` is the storage available; `wanted` is how many frames to actually keep, which the
    //: settings may make smaller. Keeping them separate is what lets the pre-roll duration change
    //: at runtime without resizing anything.
    PreRollRing(std::size_t capacity, std::size_t wanted)
        : capacity_(capacity), wanted_(wanted > capacity ? capacity : wanted) {}

    std::size_t capacity() const { return capacity_; }
    std::size_t wanted() const { return wanted_; }
    std::size_t held() const { return held_; }

    void setWanted(std::size_t wanted) {
        wanted_ = wanted > capacity_ ? capacity_ : wanted;
        if (held_ > wanted_) held_ = wanted_;
    }

    void clear() {
        next_ = 0;
        held_ = 0;
    }

    //: The slot the next frame should be written to, then advance. Returns `capacity()` when the
    //: ring is disabled (`wanted == 0`), which is not a valid slot -- the caller must not write.
    std::size_t writeSlot() {
        if (wanted_ == 0 || capacity_ == 0) return capacity_;
        const std::size_t slot = next_;
        next_ = (next_ + 1) % capacity_;
        if (held_ < wanted_) ++held_;
        return slot;
    }

    //: The slot holding the `index`-th oldest frame, for `index` in `[0, held())`. Oldest first,
    //: because that is the order the audio happened in and the only order it can be replayed in.
    std::size_t readSlot(std::size_t index) const {
        if (index >= held_ || capacity_ == 0) return capacity_;
        const std::size_t oldest = (next_ + capacity_ - held_) % capacity_;
        return (oldest + index) % capacity_;
    }

  private:
    std::size_t capacity_;
    std::size_t wanted_;
    std::size_t next_ = 0;
    std::size_t held_ = 0;
};

}  // namespace roboface

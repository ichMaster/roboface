#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

//: How much of a capture the recorder has certainly finished writing, tracked by elapsed time.
//
// `M5.Mic.record` **queues** a buffer; the DMA fills it afterwards. Returning true means the
// request was accepted, not that the samples are there. Anything that reads a buffer on the
// strength of that return value -- or on a queue depth dropping -- can read memory the recorder is
// still writing into, and the result is audio with the right length, the right level and no
// intelligible speech in it.
//
// So the recorder's progress is not asked for; it is *derived*. Capture runs at a known rate, so
// after `elapsed` milliseconds at most `elapsed * rate / 1000` samples can exist, and anything
// below both that and what was actually queued is safe to read. This is the reference project's
// pattern, arrived at there for the same reason.
class TimedCapture {
public:
    explicit TimedCapture(std::uint32_t sample_rate) : rate_(sample_rate) {}

    //: A new window. `now_ms` is the moment capture started.
    void start(std::uint32_t now_ms) {
        started_ms_ = now_ms;
        queued_ = 0;
        sent_ = 0;
    }

    //: `count` more samples handed to the recorder.
    void queued(std::size_t count) { queued_ += count; }

    //: `count` more samples read out and sent onward.
    void sent(std::size_t count) { sent_ += count; }

    std::size_t queuedSamples() const { return queued_; }
    std::size_t sentSamples() const { return sent_; }

    //: Samples queued but not yet sent -- what the ring must have room for.
    std::size_t pending() const { return queued_ - sent_; }

    //: The furthest point the recorder can have reached: never beyond what was queued, and never
    //: beyond what the clock allows.
    std::size_t filled(std::uint32_t now_ms) const {
        const std::uint32_t elapsed = now_ms - started_ms_;
        const std::size_t by_clock =
            static_cast<std::size_t>(static_cast<std::uint64_t>(elapsed) * rate_ / 1000u);
        return by_clock < queued_ ? by_clock : queued_;
    }

    //: Whether a whole `chunk` is confirmed filled and still unsent.
    bool readable(std::uint32_t now_ms, std::size_t chunk) const {
        return sent_ + chunk <= filled(now_ms);
    }

private:
    std::uint32_t rate_;
    std::uint32_t started_ms_ = 0;
    std::size_t queued_ = 0;
    std::size_t sent_ = 0;
};

}  // namespace roboface

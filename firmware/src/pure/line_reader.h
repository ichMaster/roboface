// Accumulates serial bytes into lines.
//
// The USB serial console is v0.3's debug channel and its only turn source (ROADMAP §v0.3), so this
// is the path every typed message takes. It is pure, and therefore tested on a host, because the
// alternative is discovering its edge cases by typing at a board.
//
// **Bounded, always.** A paste of a megabyte into a terminal must not be able to exhaust 320 KB of
// RAM. At the limit the line is truncated and marked, rather than grown or silently dropped: a
// person who pasted too much should get a short answer and a warning, not a reboot.

#pragma once

#include <cstddef>
#include <string>

namespace roboface {

class LineReader {
  public:
    // Comfortably longer than anything typed at a prompt, and far below both the 64 KiB wire cap
    // and anything that would threaten RAM.
    static constexpr std::size_t kDefaultMaxLine = 512;

    explicit LineReader(std::size_t max_line = kDefaultMaxLine)
        : max_line_(max_line == 0 ? 1 : max_line) {
        buffer_.reserve(max_line_ > 128 ? 128 : max_line_);
    }

    struct Line {
        bool complete = false;   // a terminator was seen
        bool truncated = false;  // the limit was hit; `text` is the first max_line bytes
        std::string text;
    };

    // Feed one byte. Returns a completed line when a terminator arrives.
    //
    // `\r\n` and `\n` both end a line, and a lone `\r` is ignored rather than treated as a
    // terminator of its own -- a terminal sending CRLF must not produce an empty line after every
    // real one.
    Line feed(char byte) {
        Line line;

        if (byte == '\r') return line;

        if (byte == '\n') {
            if (buffer_.empty() && !overflowed_) return line;  // blank line: nothing to send
            line.complete = true;
            line.truncated = overflowed_;
            line.text = buffer_;
            buffer_.clear();
            overflowed_ = false;
            return line;
        }

        if (buffer_.size() >= max_line_) {
            // Keep consuming to the terminator so the *next* line starts clean; a reader that
            // stopped here would splice the tail of an over-long paste onto whatever came after.
            overflowed_ = true;
            return line;
        }

        buffer_.push_back(byte);
        return line;
    }

    // Feed a run of bytes, returning every completed line in order.
    template <typename Sink>
    void feed(const char* data, std::size_t length, Sink&& sink) {
        for (std::size_t index = 0; index < length; ++index) {
            Line line = feed(data[index]);
            if (line.complete) sink(line);
        }
    }

    void clear() {
        buffer_.clear();
        overflowed_ = false;
    }

    std::size_t pending() const { return buffer_.size(); }
    std::size_t maxLine() const { return max_line_; }

  private:
    std::size_t max_line_;
    std::string buffer_;
    bool overflowed_ = false;
};

}  // namespace roboface

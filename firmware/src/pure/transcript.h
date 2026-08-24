// Turning a stream of reply fragments into lines that fit the screen.
//
// **Pure**: header-only, `namespace roboface`, no <M5Unified.h>, no <Arduino.h>. The `native`
// environment compiles only `src/pure/`, so an accidental hardware include fails `pio test -e
// native` immediately rather than being discovered by the next person who tries to test this
// without a board.
//
// Two things here are easy to get wrong and expensive to notice on a 320x240 panel:
//
// **Columns are codepoints, not bytes.** The product answers in Ukrainian, where every letter is
// two bytes of UTF-8. A byte-based wrap would put half as many letters on each line as it thought
// it had, and would cut characters in half at the break -- which does not render as a wrong line
// break, it renders as a replacement glyph. So every measurement below counts characters.
//
// **Deltas are fragments, not lines.** A `reply` frame carries whatever the model emitted, which
// is usually a partial word and can be a partial *character*: the server streams bytes, and a
// two-byte letter can straddle two frames. Wrapping each fragment on its own would turn every
// fragment boundary into a line break, and rendering a straddled character would show a box. So
// fragments accumulate into one string, an incomplete trailing sequence is held back until the
// rest of it arrives, and the wrap runs over the whole.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pure/layout.h"

namespace roboface {

// --- UTF-8 primitives ------------------------------------------------------------------

// How many bytes the sequence starting with `lead` occupies; 0 if it is not a lead byte.
inline constexpr std::size_t utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

inline constexpr bool isUtf8Continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

// Codepoints, not bytes. A malformed byte counts as one character rather than aborting: this runs
// on whatever arrived over a socket, and refusing to measure a damaged string would mean refusing
// to draw anything at all.
inline std::size_t utf8Length(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t len = utf8SequenceLength(static_cast<unsigned char>(text[i]));
        if (len == 0 || i + len > text.size()) len = 1;
        i += len;
        ++count;
    }
    return count;
}

// The length of the longest prefix that contains no truncated character -- i.e. where to cut a
// buffer so that what you keep is renderable and what you hold back is the start of a character
// still in flight.
inline std::size_t utf8CompletePrefix(const std::string& text) {
    std::size_t index = text.size();
    // A UTF-8 sequence is at most four bytes, so the lead byte of any incomplete tail is within
    // the last four. Walking further back would be looking for something that cannot be there.
    for (std::size_t stepped = 0; index > 0 && stepped < 4; ++stepped) {
        --index;
        const auto byte = static_cast<unsigned char>(text[index]);
        if (isUtf8Continuation(byte)) continue;
        const std::size_t len = utf8SequenceLength(byte);
        // A malformed lead byte is not "in flight", it is simply wrong; holding it back forever
        // would stall the transcript on one bad byte.
        if (len == 0) return text.size();
        return index + len <= text.size() ? text.size() : index;
    }
    return text.size();
}

// --- Wrapping --------------------------------------------------------------------------

namespace detail {

inline void rtrimSpaces(std::string& line) {
    while (!line.empty() && line.back() == ' ') line.pop_back();
}

inline bool isBreakingSpace(unsigned char byte) {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

}  // namespace detail

// Word-wrap `text` into lines of at most `columns` characters.
//
// A word that cannot fit on the current line moves down whole; a word longer than a whole line is
// hard-broken, always on a character boundary. `\n` forces a break, so a deliberate blank line
// survives. Trailing spaces never reach a line: they would count against the width and draw
// nothing.
inline std::vector<std::string> wrapUtf8(const std::string& text, std::size_t columns) {
    std::vector<std::string> lines;
    if (columns == 0) return lines;

    std::string line;
    std::size_t line_columns = 0;

    const auto pushLine = [&] {
        detail::rtrimSpaces(line);
        lines.push_back(line);
        line.clear();
        line_columns = 0;
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);

        if (byte == '\n') {
            pushLine();
            ++i;
            continue;
        }
        if (detail::isBreakingSpace(byte)) {
            // A space at the start of a line would be an indent nobody asked for, and a space at
            // the width would be a column spent on nothing.
            if (line_columns > 0 && line_columns < columns) {
                line += ' ';
                ++line_columns;
            }
            ++i;
            continue;
        }

        // One word, measured in characters.
        const std::size_t word_start = i;
        std::size_t word_columns = 0;
        while (i < text.size()) {
            const auto b = static_cast<unsigned char>(text[i]);
            if (b == '\n' || detail::isBreakingSpace(b)) break;
            std::size_t len = utf8SequenceLength(b);
            if (len == 0 || i + len > text.size()) len = 1;
            i += len;
            ++word_columns;
        }
        const std::string word = text.substr(word_start, i - word_start);

        // Fits on a line of its own but not on what is left of this one: move it down whole.
        if (word_columns <= columns && line_columns + word_columns > columns) pushLine();

        // Emitted a character at a time, so a word longer than the whole line breaks at a
        // character boundary instead of at a byte.
        for (std::size_t k = 0; k < word.size();) {
            std::size_t len = utf8SequenceLength(static_cast<unsigned char>(word[k]));
            if (len == 0 || k + len > word.size()) len = 1;
            if (line_columns == columns) pushLine();
            line.append(word, k, len);
            ++line_columns;
            k += len;
        }
    }

    detail::rtrimSpaces(line);
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// --- The transcript --------------------------------------------------------------------

//: Lines the console keeps. Beyond this the oldest are dropped: the panel cannot show them, and a
//: buffer that grew with the conversation would be a slow leak on a device with finite PSRAM.
//: Taken from the screen geometry rather than chosen here, so the bound and the panel agree.
inline constexpr std::size_t kTranscriptMaxLines = static_cast<std::size_t>(kConsoleLines);

//: The default width, likewise from the geometry: the face area divided by the font's cell.
inline constexpr std::size_t kTranscriptColumns = static_cast<std::size_t>(kConsoleColumns);

//: A hard byte ceiling on the accumulated reply, independent of the line bound. A model that
//: streamed without stopping would otherwise grow this string forever even though only the last
//: few lines are ever drawn.
inline constexpr std::size_t kTranscriptMaxReplyBytes = 2048;

// One exchange: what was sent, and the answer as it arrives.
class Transcript {
  public:
    Transcript() = default;
    Transcript(std::size_t columns, std::size_t max_lines)
        : columns_(columns), max_lines_(max_lines) {}

    // A new turn. The previous exchange goes: the panel shows one at a time, and keeping the old
    // reply visible under a new question is how a console starts lying about what it is answering.
    void startTurn(const std::string& outgoing) {
        outgoing_ = outgoing;
        reply_.clear();
        pending_.clear();
        dirty_ = true;
    }

    // One `reply` delta. Whatever completes a character is kept; a truncated tail waits for the
    // frame that finishes it.
    void appendReply(const std::string& fragment) {
        pending_ += fragment;
        const std::size_t complete = utf8CompletePrefix(pending_);
        reply_.append(pending_, 0, complete);
        pending_.erase(0, complete);
        trimReply();
        dirty_ = true;
    }

    void clear() {
        outgoing_.clear();
        reply_.clear();
        pending_.clear();
        dirty_ = true;
    }

    const std::string& outgoing() const { return outgoing_; }
    const std::string& reply() const { return reply_; }
    bool empty() const { return outgoing_.empty() && reply_.empty(); }

    // Both return references into the cache: the renderer asks for these on every repaint, and
    // re-wrapping there meant rebuilding up to ~80 strings per frame while a reply streamed --
    // roughly 2400 short-lived allocations a second on a heap that is not compacted.
    const std::vector<std::string>& outgoingLines() const {
        rewrapIfNeeded();
        return outgoing_lines_;
    }
    const std::vector<std::string>& replyLines() const {
        rewrapIfNeeded();
        return reply_view_;
    }

    // Everything, bounded. What the renderer draws is `outgoingLines()` then `replyLines()`, so
    // the two can be styled differently; this is the whole-transcript view the bound is stated in.
    // Trimmed from the *combined* list, which is why the untrimmed reply wrap is cached too.
    std::vector<std::string> lines() const {
        rewrapIfNeeded();
        std::vector<std::string> all = outgoing_lines_;
        for (const std::string& line : reply_wrapped_) all.push_back(line);
        return lastLines(all);
    }

    std::size_t columns() const { return columns_; }
    std::size_t maxLines() const { return max_lines_; }

  private:
    // The wrap is pure and the inputs only change on append, so it is computed once per change
    // rather than once per repaint. `mutable` because this is a cache of a value the object
    // already logically has -- callers see a const object whose answers never change without a
    // mutation. The risk a cache introduces is staleness, so every mutator sets `dirty_` and the
    // host tests assert the invalidation rather than only the contents.
    void rewrapIfNeeded() const {
        if (!dirty_) return;
        outgoing_lines_ = wrapUtf8(outgoing_, columns_);
        reply_wrapped_ = wrapUtf8(reply_, columns_);
        reply_view_ = lastLines(reply_wrapped_);
        dirty_ = false;
    }

    std::vector<std::string> lastLines(std::vector<std::string> all) const {
        if (all.size() > max_lines_) {
            all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() - max_lines_));
        }
        return all;
    }

    void trimReply() {
        if (reply_.size() <= max_reply_bytes_) return;
        std::size_t drop = reply_.size() - max_reply_bytes_;
        // Advance to a character boundary, so trimming the front never leaves a dangling
        // continuation byte at the start of the string.
        while (drop < reply_.size() && isUtf8Continuation(static_cast<unsigned char>(reply_[drop]))) {
            ++drop;
        }
        reply_.erase(0, drop);
    }

    std::size_t columns_ = kTranscriptColumns;
    std::size_t max_lines_ = kTranscriptMaxLines;
    std::size_t max_reply_bytes_ = kTranscriptMaxReplyBytes;
    std::string outgoing_;
    std::string reply_;
    std::string pending_;

    mutable bool dirty_ = true;
    mutable std::vector<std::string> outgoing_lines_;
    mutable std::vector<std::string> reply_wrapped_;  // untrimmed, for `lines()`
    mutable std::vector<std::string> reply_view_;     // trimmed to the bound, for the renderer
};

}  // namespace roboface

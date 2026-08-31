// Choosing a face.
//
// DEVICE_UI §Input: *"a hold is PTT from the first millisecond — that is the common case and it must
// not wait. Only a hold that passes 1.2 s with no speech detected converts."* `pure/touch.h` has
// produced `kHeldSilent` since v2.4; this is its consumer.
//
// **It was slide-to-choose, and that was wrong on the hardware.** The first version followed the
// obvious reading of "carousel": hold, keep the finger down, slide along a strip of dots, release
// on one. It works in a description and badly in a hand — the dots are 5 px on a 320 px panel, the
// finger covers the thing it is selecting, and the whole gesture has to be performed without
// letting go. Tried on the board, it was called *"туго"*: stiff.
//
// So the carousel is a **modal picker driven by taps**, and the finger is free the moment the strip
// appears:
//
//     ┌──────────────────────────────────┐
//     │  ✕                               │   the top band cancels
//     ├────────┬───────────────┬─────────┤
//     │        │               │         │
//     │   ◀    │   the face,   │    ▶    │   tap an arrow to step
//     │        │   previewed   │         │   tap the face to accept
//     │        │               │         │
//     ├────────┴───────────────┴─────────┤
//     │        ●  ○  ○  ○  ○             │   tap a dot to jump straight to it
//     └──────────────────────────────────┘
//
// Every target is at least 76 px wide. The dots stay, because a strip of five with one lit is the
// only thing on the screen that says *where you are* — but nothing requires you to hit one any more.
//
// Pure: header-only, `namespace roboface`, no clock of its own -- time arrives as a parameter.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/layout.h"

namespace roboface {

//: How long the confirmation toast stands after a skin changes. DEVICE_UI calls it a *whisper*
//: toast: long enough to read a five-letter word, short enough to be gone before it is annoying.
inline constexpr uint32_t kToastMs = 2200;

//: How long an untouched carousel stands before it gives up.
//:
//: **A picker that stays open forever is a device that has stopped being a companion.** The gesture
//: that opens it can be performed by accident — a hand resting on the glass for a moment while the
//: room is quiet — and the face is hidden behind arrows until someone deals with it. Twenty seconds
//: is far longer than choosing takes and far shorter than a person's patience with a device that
//: appears to have frozen.
inline constexpr uint32_t kCarouselIdleMs = 20000;

//: The dot strip's geometry, in the bottom band.
inline constexpr int kCarouselDotSpacing = 34;
inline constexpr int kCarouselDotRadius = 5;
inline constexpr int kCarouselSelectedRadius = 8;
inline constexpr int kCarouselCentreY = kFaceBottom + kBandHeight / 2;

//: The arrows' columns, inside the face area. 76 px each -- three fingertips wide, because this
//: replaced a 5 px target and halving the problem would not have been worth the change.
inline constexpr int kCarouselArrowWidth = 76;
inline constexpr int kCarouselPrevRight = kFaceLeft + kCarouselArrowWidth;   // 104
inline constexpr int kCarouselNextLeft = kFaceRight - kCarouselArrowWidth;   // 216

//: What a tap on an open carousel means.
enum class CarouselZone : uint8_t {
    kNone,
    kPrev,     // ◀ the left column of the face area
    kNext,     // ▶ the right column
    kConfirm,  // the previewed face itself: "yes, this one"
    kCancel,   // the top band -- big, and away from everything else
    kDot,      // a dot in the strip: jump straight to that face
};

//: Where a dot is drawn. The drawing code asks rather than computing, so the hit test and the
//: pixels cannot drift -- the same rule `touch.h` follows against `layout.h`.
inline constexpr int carouselDotX(std::size_t index, std::size_t count) {
    const int span = static_cast<int>(count - 1) * kCarouselDotSpacing;
    return kScreenWidth / 2 - span / 2 + static_cast<int>(index) * kCarouselDotSpacing;
}

//: Which dot a coordinate is nearest, or `count` when it is not in the strip at all.
inline constexpr std::size_t carouselDotAt(int x, int y, std::size_t count) {
    if (count == 0 || y < kFaceBottom) return count;

    const int span = static_cast<int>(count - 1) * kCarouselDotSpacing;
    const int first_x = kScreenWidth / 2 - span / 2;
    const int left = first_x - kCarouselDotSpacing / 2;
    const int right = first_x + span + kCarouselDotSpacing / 2;
    if (x < left || x > right) return count;

    const int offset = x - first_x + kCarouselDotSpacing / 2;
    if (offset < 0) return 0;
    const auto index = static_cast<std::size_t>(offset / kCarouselDotSpacing);
    return index >= count ? count - 1 : index;
}

//: What a tap at this coordinate means while the carousel is open.
//:
//: **Total over the screen**: every pixel answers something, and nothing falls through to the
//: gesture classifier. A modal picker that let some taps past would tickle the face from behind its
//: own arrows.
inline constexpr CarouselZone carouselZoneAt(int x, int y, std::size_t count) {
    if (y < kFaceTop) return CarouselZone::kCancel;
    if (y >= kFaceBottom) {
        return carouselDotAt(x, y, count) < count ? CarouselZone::kDot : CarouselZone::kCancel;
    }
    if (x < kCarouselPrevRight) return CarouselZone::kPrev;
    if (x >= kCarouselNextLeft) return CarouselZone::kNext;
    return CarouselZone::kConfirm;
}

//: What the carousel wants to happen.
enum class CarouselOutcome : uint8_t {
    kNothing,
    kOpened,
    kMoved,     // the selection changed; preview it
    kConfirmed,
    kCancelled,
};

// The carousel's state.
//
// **It previews rather than waiting for a confirmation**, which is the one piece of generosity in a
// file otherwise built around not stealing a gesture: a strip of dots says nothing about what the
// faces look like, and choosing blind is not choosing. A cancel puts back the one that was worn.
class Carousel {
  public:
    CarouselOutcome open(std::size_t current, std::size_t count, uint32_t now_ms) {
        if (open_ || count == 0) return CarouselOutcome::kNothing;
        open_ = true;
        count_ = count;
        restore_ = current;
        selected_ = current;
        touched_at_ms_ = now_ms;
        // **The finger that opened this is still down.** Its release must not be read as a tap on
        // whatever is now underneath it -- which, since the gesture is a hold on the face, is the
        // confirm zone. Without this, every carousel would close the instant it appeared.
        awaiting_release_ = true;
        return CarouselOutcome::kOpened;
    }

    //: The finger left the glass. Only interesting for the press that opened the carousel.
    void released() { awaiting_release_ = false; }

    //: A tap landed. Returns what it meant.
    CarouselOutcome tapped(int x, int y, uint32_t now_ms) {
        if (!open_ || awaiting_release_) return CarouselOutcome::kNothing;
        touched_at_ms_ = now_ms;

        switch (carouselZoneAt(x, y, count_)) {
            case CarouselZone::kPrev:
                // **Wrapping, both ways.** Five faces and no wrap means the person at one end has
                // to travel the whole strip to reach the other -- with a picker this small, the
                // ends are where you most often are.
                selected_ = selected_ == 0 ? count_ - 1 : selected_ - 1;
                return CarouselOutcome::kMoved;

            case CarouselZone::kNext:
                selected_ = selected_ + 1 >= count_ ? 0 : selected_ + 1;
                return CarouselOutcome::kMoved;

            case CarouselZone::kDot: {
                const std::size_t dot = carouselDotAt(x, y, count_);
                if (dot == selected_) return CarouselOutcome::kNothing;
                selected_ = dot;
                return CarouselOutcome::kMoved;
            }

            case CarouselZone::kConfirm:
                open_ = false;
                return CarouselOutcome::kConfirmed;

            case CarouselZone::kCancel:
                open_ = false;
                selected_ = restore_;
                return CarouselOutcome::kCancelled;

            case CarouselZone::kNone:
                break;
        }
        return CarouselOutcome::kNothing;
    }

    //: Time passing. Closes an untouched picker rather than leaving the face buried under arrows.
    CarouselOutcome tick(uint32_t now_ms) {
        if (!open_ || now_ms - touched_at_ms_ < kCarouselIdleMs) return CarouselOutcome::kNothing;
        open_ = false;
        selected_ = restore_;
        return CarouselOutcome::kCancelled;
    }

    //: Give up without a decision -- the device changed state under the finger, or the socket died.
    //: **Restores rather than keeping the preview**: a face chosen by an interrupted gesture is a
    //: face nobody chose.
    CarouselOutcome abandon() {
        if (!open_) return CarouselOutcome::kNothing;
        open_ = false;
        selected_ = restore_;
        return CarouselOutcome::kCancelled;
    }

    bool isOpen() const { return open_; }
    std::size_t selected() const { return selected_; }
    std::size_t count() const { return count_; }

  private:
    bool open_ = false;
    bool awaiting_release_ = false;
    std::size_t count_ = 0;
    std::size_t selected_ = 0;
    std::size_t restore_ = 0;
    uint32_t touched_at_ms_ = 0;
};

}  // namespace roboface

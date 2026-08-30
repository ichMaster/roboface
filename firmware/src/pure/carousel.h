// Choosing a face with one finger.
//
// DEVICE_UI §Input: *"a hold is PTT from the first millisecond — that is the common case and it must
// not wait. Only a hold that passes 1.2 s with no speech detected converts."* `pure/touch.h` has
// produced `kHeldSilent` since v2.4 and nothing consumed it; this is its consumer, and writing it
// is also the test of whether that gesture should exist at all.
//
// **The gesture's cost is what shapes this file.** Opening the carousel means a listening window was
// opened and then taken away — the person held the screen, the microphone came on, and 1.2 s later
// the device decided they meant something else. So every rule here leans toward *not* stealing the
// gesture, and toward making a mistaken open cheap to escape:
//
//   * speech at any point during the hold keeps it a PTT hold, permanently — `touch.h`'s rule
//   * releasing outside the strip cancels rather than choosing the nearest
//   * cancelling restores the face that was worn, not the one under the finger when it opened
//
// Pure: header-only, `namespace roboface`, no clock of its own -- time arrives as a parameter.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/layout.h"

namespace roboface {

//: How long the confirmation toast stands after a skin changes. DEVICE_UI calls it a *whisper*
//: toast: long enough to read a five-letter word, short enough that it is gone before it is
//: annoying. It fades by the same ~3 s settle rule the rest of chrome uses.
inline constexpr uint32_t kToastMs = 2200;

//: The dot strip's geometry, in the bottom band. Derived from `layout.h` rather than picked, for
//: the reason that header exists: the rule "chrome never blocks the face" is arithmetic, and it can
//: only be proven on a laptop if the numbers are here.
inline constexpr int kCarouselDotSpacing = 34;
inline constexpr int kCarouselDotRadius = 5;
inline constexpr int kCarouselSelectedRadius = 8;

//: Where the strip sits vertically: the middle of the bottom band.
inline constexpr int kCarouselCentreY = kFaceBottom + kBandHeight / 2;

//: How far outside the strip a finger may stray before releasing counts as a cancel. Generous,
//: because the alternative -- snapping to the nearest dot -- means a person who has changed their
//: mind cannot say so, and the gesture already cost them a listening window.
inline constexpr int kCarouselCancelMarginPx = 40;

//: Which dot a coordinate is over, or `count` when it is outside the strip.
//:
//: The strip is centred on the screen and sized by however many skins there are: `kSkinCount` is
//: not baked in here, so a sixth face widens the strip rather than needing a new number.
inline constexpr std::size_t carouselDotAt(int x, int y, std::size_t count) {
    if (count == 0) return 0;

    const int span = static_cast<int>(count - 1) * kCarouselDotSpacing;
    const int first_x = kScreenWidth / 2 - span / 2;

    if (y < kFaceBottom - kCarouselCancelMarginPx) return count;
    const int left = first_x - kCarouselDotSpacing / 2 - kCarouselCancelMarginPx;
    const int right = first_x + span + kCarouselDotSpacing / 2 + kCarouselCancelMarginPx;
    if (x < left || x > right) return count;

    // Nearest dot, by rounding rather than by a loop: the strip is evenly spaced, so this is the
    // same answer with none of the ways a loop can be off by one at the ends.
    const int offset = x - first_x + kCarouselDotSpacing / 2;
    if (offset < 0) return 0;
    const auto index = static_cast<std::size_t>(offset / kCarouselDotSpacing);
    return index >= count ? count - 1 : index;
}

//: Where a dot is drawn. The drawing code asks rather than computing, so the hit test and the
//: pixels cannot drift -- the same reason `touch.h` derives its zones from `layout.h`.
inline constexpr int carouselDotX(std::size_t index, std::size_t count) {
    const int span = static_cast<int>(count - 1) * kCarouselDotSpacing;
    return kScreenWidth / 2 - span / 2 + static_cast<int>(index) * kCarouselDotSpacing;
}

//: What the carousel wants to happen.
enum class CarouselOutcome : uint8_t {
    kNothing,
    kOpened,
    kMoved,     // the selection changed under the finger; preview it
    kConfirmed,
    kCancelled,
};

// The carousel's state: open or not, and which dot the finger is over.
//
// **It previews rather than waiting for a release**, which is the one piece of generosity in a file
// otherwise built around not stealing a gesture: a strip of five dots says nothing about what the
// faces look like, and choosing blind is not choosing. So the face changes as the finger slides,
// and a cancel puts back the one that was worn.
class Carousel {
  public:
    //: Open it, remembering what was worn so a cancel can restore it.
    CarouselOutcome open(std::size_t current, std::size_t count, uint32_t now_ms) {
        if (open_ || count == 0) return CarouselOutcome::kNothing;
        open_ = true;
        count_ = count;
        restore_ = current;
        selected_ = current;
        opened_at_ms_ = now_ms;
        return CarouselOutcome::kOpened;
    }

    //: The finger moved. Returns `kMoved` only when the selection actually changed -- a preview
    //: reissued every frame would redraw the face fifty times a second for a finger sitting still.
    CarouselOutcome moved(int x, int y) {
        if (!open_) return CarouselOutcome::kNothing;
        const std::size_t dot = carouselDotAt(x, y, count_);
        outside_ = dot >= count_;
        if (outside_ || dot == selected_) return CarouselOutcome::kNothing;
        selected_ = dot;
        return CarouselOutcome::kMoved;
    }

    //: The finger left. Outside the strip cancels; on it confirms.
    CarouselOutcome released() {
        if (!open_) return CarouselOutcome::kNothing;
        open_ = false;
        if (outside_) {
            selected_ = restore_;
            return CarouselOutcome::kCancelled;
        }
        return CarouselOutcome::kConfirmed;
    }

    //: Give up without a release -- the device changed state under the finger, or the socket died.
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
    uint32_t openedAtMs() const { return opened_at_ms_; }

  private:
    bool open_ = false;
    bool outside_ = false;
    std::size_t count_ = 0;
    std::size_t selected_ = 0;
    std::size_t restore_ = 0;
    uint32_t opened_at_ms_ = 0;
};

}  // namespace roboface

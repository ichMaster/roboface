// A handful of mouths, and which one to show.
//
// The mouth does not follow the voice continuously -- it takes one of a few fixed shapes. That is
// how hand-drawn animation has always done it, and it is better here for two separate reasons that
// happen to agree.
//
// **It looks more like speech.** A mouth scaled smoothly by loudness reads as a jaw being pulled
// open and shut. Real speech does not vary its opening continuously either: it moves between a
// small number of positions, quickly.
//
// **It costs almost nothing.** The face is redrawn when the *shape* changes, not when the level
// does. A continuous mouth changes by some amount on every frame and so redraws on every frame;
// four shapes change perhaps six times a second, and every redraw is a push of the whole sprite.
//
// The hysteresis is what makes the second reason true. Without it a level sitting near a threshold
// flips between two shapes on adjacent frames -- a flutter that looks wrong *and* costs more than
// the smooth version it replaced.
//
// Four is not a magic number. The table below is the whole definition: a fifth shape is one row.

#pragma once

#include <cstddef>
#include <cstdint>

namespace roboface {

//: The shapes, in order of opening. `kClosed` is the resting mouth -- whatever expression the face
//: is wearing, unmodified.
enum class MouthFrame : uint8_t {
    kClosed,
    kAjar,
    kHalf,
    kWide,
    kOpen,
    kCount,
};

//: What to draw: how far open, and how wide. Two numbers rather than one because a mouth that only
//: changes height is a jaw hinging, and it reads as a puppet. Speech changes both.
struct MouthPose {
    float open = 0.0f;   // 0..1 of the geometry's opening travel
    float width = 1.0f;  // a scale on the mouth's own width; 1.0 is neutral
};

//: One shape: the level it opens at, the level it falls back below, and how far it moves the mouth.
//:
//: `opens_at` and `closes_at` are deliberately different -- that gap is the hysteresis. Measured
//: against a real reply, the playback level crosses any single threshold several times per
//: syllable.
//:
//: The travels are not evenly spaced. Most of speech lives between closed and ajar, so that step is
//: small; fully open is for emphasis and is rare, so it is worth a large one.
struct MouthStep {
    float opens_at;
    float closes_at;
    float travel;
    //: Two widths for the same opening. Which one is used alternates from syllable to syllable --
    //: see `LipSync::rounded()`. A spread mouth and a pursed one at the same height are the two
    //: shapes a viewer reads as different sounds, and alternating them is what makes four shapes
    //: look like speech instead of a jaw hinging open and shut.
    float spread_width;
    float round_width;
};

//: Index by `MouthFrame`. Adding a shape is adding a row and an enum entry, in that order.
//: `travel` is how far **open** the mouth goes, 0..1 -- not how far it curves.
//:
//: That distinction was a real mistake before it was a design note. Driving the mouth through the
//: recipe's `mouth_curve` meant a face already smiling at 0.70 hit the 1.0 ceiling on the first
//: shape and moved two pixels: the lip-sync ran perfectly and was invisible. A talking mouth
//: **opens**; it does not smile harder.
//: The thresholds are spread across the range the signal **actually uses**, which was measured
//: rather than assumed: a reply drives the playback level over 0..95%, and the first version of this
//: table put its top step at 0.30. The mouth therefore sat on the top shape for almost the entire
//: reply and changed four times in ten seconds. Spread over the real range it changes twenty to
//: forty times, which is speech.
inline constexpr MouthStep kMouthSteps[static_cast<std::size_t>(MouthFrame::kCount)] = {
    //  opens  closes  open  spread  round
    {0.00f, 0.00f, 0.00f, 1.00f, 1.00f},  // kClosed -- the floor; never "opens at" anything
    {0.06f, 0.04f, 0.30f, 0.95f, 0.60f},  // kAjar
    {0.20f, 0.15f, 0.55f, 1.10f, 0.55f},  // kHalf
    {0.40f, 0.32f, 0.80f, 1.25f, 0.62f},  // kWide
    {0.65f, 0.55f, 1.00f, 1.15f, 0.72f},  // kOpen  -- emphasis
};

class LipSync {
  public:
    //: Feed the playback level, 0..1. Returns the shape to draw.
    //:
    //: Rises to the highest shape the level has reached, falls only when it drops below *that
    //: shape's* closing threshold. Written as a scan rather than a switch so the table stays the
    //: only place the shapes are defined.
    MouthFrame feed(float level) {
        auto index = static_cast<std::size_t>(frame_);

        // Fall first: a level that has dropped past this shape's floor cannot also be opening.
        while (index > 0 && level < kMouthSteps[index].closes_at) --index;
        // Then rise as far as the level reaches.
        while (index + 1 < static_cast<std::size_t>(MouthFrame::kCount) &&
               level >= kMouthSteps[index + 1].opens_at) {
            ++index;
        }

        // Every time the mouth shuts, the next opening takes the other width. That is the whole
        // alternation: no phonetics, no timer -- the gaps between syllables are already in the
        // signal, so counting them is free and lands on roughly the right rhythm.
        const auto next = static_cast<MouthFrame>(index);
        if (next == MouthFrame::kClosed && frame_ != MouthFrame::kClosed) rounded_ = !rounded_;
        frame_ = next;
        return frame_;
    }

    MouthFrame frame() const { return frame_; }

    //: Which width variant this syllable is using.
    bool rounded() const { return rounded_; }

    //: The current shape, ready to draw.
    MouthPose pose() const {
        const auto& step = kMouthSteps[static_cast<std::size_t>(frame_)];
        return MouthPose{step.travel, rounded_ ? step.round_width : step.spread_width};
    }

    //: Shut the mouth. Called when speech ends -- left alone it would hold the shape of the last
    //: syllable for as long as the device sat idle afterwards, which is a stranger thing to look at
    //: than a face that never moved.
    void reset() { frame_ = MouthFrame::kClosed; }

  private:
    MouthFrame frame_ = MouthFrame::kClosed;
    bool rounded_ = false;
};

//: How much a shape adds to `FaceRecipe::mouth_curve`.
inline constexpr float travelFor(MouthFrame frame) {
    const auto index = static_cast<std::size_t>(frame);
    if (index >= static_cast<std::size_t>(MouthFrame::kCount)) return 0.0f;
    return kMouthSteps[index].travel;
}

}  // namespace roboface

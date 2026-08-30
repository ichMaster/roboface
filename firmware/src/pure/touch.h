// Where a finger landed, and what it did.
//
// `pure/ptt.h` already answers one question about the panel — is this a hold or a tap — because v1.2
// needed exactly that and nothing more. This is the rest of the vocabulary DEVICE_UI §Input names,
// and it **extends** that rather than replacing it: press-and-hold is still the control gesture and
// is still classified there. What arrives here is affection.
//
// **The zones come from `layout.h`, not from fresh numbers.** A poke is a press that lands on an
// eye, so where the eyes are drawn and where a poke is detected are the same fact; writing the
// coordinates twice would let a change to the face move one without the other, and the symptom would
// be a poke that works everywhere except on the eyes.
//
// Pure: header-only, `namespace roboface`, no M5GFX, no clock of its own -- time arrives as a
// parameter, which is what lets a whole gesture be replayed on a laptop in microseconds.

#pragma once

#include <cstdint>

#include "pure/layers.h"
#include "pure/layout.h"
#include "pure/ptt.h"

namespace roboface {

//: Where on the face a touch landed. `kOutside` is the chrome bands, which belong to the UI rather
//: than to the character -- a touch there is not affection.
enum class TouchZone : uint8_t {
    kOutside,
    kMicButton,  // the one control target on the screen: top-left of the upper band
    kForehead,
    kEye,
    kCheek,
    kCount,
};

//: What the finger did. DEVICE_UI §Input's affection vocabulary, plus the two control outcomes the
//: hold produces -- which live here because they are decided by the *same* stream and splitting them
//: across two classifiers would mean two things watching one finger.
enum class TouchGesture : uint8_t {
    kNone,
    kTap,         // pressed and released, briefly, once
    kMultiTap,    // taps in quick succession -- the count rides along
    kStroke,      // held while moving across the face
    kPokeEye,     // a press that landed on an eye
    kLongPress,   // held past the PTT threshold -- control, not affection
    kHeldSilent,  // held past 1.2 s with no speech: v2.6 turns this into the carousel
    kMicToggle,   // the microphone button was tapped -- control, not affection
    kCount,
};

inline constexpr const char* toString(TouchGesture gesture) {
    switch (gesture) {
        case TouchGesture::kNone: return "none";
        case TouchGesture::kTap: return "tap";
        case TouchGesture::kMultiTap: return "multi_tap";
        case TouchGesture::kStroke: return "stroke";
        case TouchGesture::kPokeEye: return "poke_eye";
        case TouchGesture::kLongPress: return "long_press";
        case TouchGesture::kHeldSilent: return "held_silent";
        case TouchGesture::kMicToggle: return "mic_toggle";
        case TouchGesture::kCount: break;
    }
    return "none";
}

//: How long after a tap another one still counts as part of the same gesture.
//:
//: **The window is the design decision here.** Too short and a deliberate double tap arrives as two
//: singles, so the character reacts twice instead of once and the person's intent is lost. Too long
//: and two unrelated taps merge, so a touch now inherits the meaning of one from a second ago.
//:
//: 400 ms: a comfortable double-tap is 150-300 ms apart, and two deliberate separate touches are
//: rarely closer than half a second.
inline constexpr uint32_t kMultiTapWindowMs = 400;

//: How long after a reported gesture the next one is suppressed.
//:
//: **The count keeps rising; only the reporting is throttled.** That distinction is the whole
//: design: a person drumming on the face should not produce twenty reflexes and twenty frames on
//: the wire, but they also should not have their enthusiasm discarded. So a burst of taps yields
//: one reaction every quarter second -- and each one is *stronger* than the last, because the taps
//: in between were counted.
//:
//: 250 ms is slower than a person can deliberately double-tap and faster than they can notice a
//: reaction being withheld.
inline constexpr uint32_t kGestureRefractoryMs = 250;

//: How far a finger must travel while held before it is a stroke rather than a hold that wobbled.
//: A finger resting on glass drifts a few pixels; 40 px is about a fingertip's width at this size,
//: so it cannot be reached without meaning it.
inline constexpr int kStrokeTravelPx = 40;

//: A hold that passes this **without speech** stops being PTT and becomes the carousel's gesture.
//: DEVICE_UI §Input: *"a hold is PTT from the first millisecond -- that is the common case and it
//: must not wait. Only a hold that passes 1.2 s with no speech detected converts."*
inline constexpr uint32_t kCarouselHoldMs = 1200;

//: Which zone a coordinate is in.
//:
//: Derived from the face geometry rather than from constants of its own: the eye rectangles are the
//: ones `layout.h` draws, widened by a margin because a finger is far larger than a pupil and a poke
//: that required pixel accuracy would simply never fire.
inline constexpr TouchZone zoneAt(int x, int y, const FaceGeometry& geometry = {}) {
    // Checked first, and it cannot overlap the face: `inMicButton` lives entirely above `kFaceTop`.
    // The order still matters for readability -- a reader should not have to prove the two are
    // disjoint to know which one wins.
    if (inMicButton(x, y)) return TouchZone::kMicButton;

    if (x < kFaceLeft || x >= kFaceRight || y < kFaceTop || y >= kFaceBottom) {
        return TouchZone::kOutside;
    }

    //: A fingertip is about this wide on a 320x240 panel at arm's length.
    constexpr int kFingerPx = 24;

    const int eye_y = geometry.centre_y + geometry.eye_offset_y;
    const int eye_half = geometry.eye_spacing / 2;
    for (const int centre : {geometry.centre_x - eye_half, geometry.centre_x + eye_half}) {
        const bool near_x = x > centre - geometry.eye_half_width - kFingerPx &&
                            x < centre + geometry.eye_half_width + kFingerPx;
        const bool near_y = y > eye_y - geometry.eye_open_height - kFingerPx &&
                            y < eye_y + geometry.eye_open_height + kFingerPx;
        if (near_x && near_y) return TouchZone::kEye;
    }

    // Above the eyes is forehead; the rest of the face is cheek. Two zones rather than four,
    // because DEVICE_UI gives them the same reaction and a distinction nothing acts on is a
    // distinction that will drift.
    return y < eye_y ? TouchZone::kForehead : TouchZone::kCheek;
}

//: One sample from the panel. `down` false means the finger left.
struct TouchSample {
    bool down = false;
    int x = 0;
    int y = 0;
    uint32_t at_ms = 0;
    //: How many fingers are on the glass. **More than one is never affection.**
    //:
    //: Mute moved off the two-finger tap in v2.4 -- measured on the board, the CoreS3's panel
    //: reports a single point (`peak fingers=1` on every two-fingered tap) whatever the FT6336U's
    //: datasheet says it can do. So nothing acts on two fingers any more.
    //:
    //: This guard stays anyway, and deliberately: a second finger landing is still not a caress,
    //: and on the day a board *does* report two points the character should not be delighted by
    //: whatever the person was actually doing.
    uint8_t fingers = 1;
};

//: What the classifier decided this sample, and where.
struct TouchResult {
    TouchGesture gesture = TouchGesture::kNone;
    TouchZone zone = TouchZone::kOutside;
    //: For `kMultiTap`, how many taps have accumulated. 1 for a plain tap.
    uint8_t count = 0;
};

// The affection half of the touch vocabulary.
//
// **`PushToTalk` is still the control half and is not duplicated here.** It owns whether a hold
// opens a listening window, which is a decision about the microphone; this owns what the finger
// *meant*, which is a decision about the character. Two classifiers over one finger is a real cost
// -- they can disagree -- and it is paid deliberately: the alternative is one class that decides
// both, and the day PTT changes it would silently change what a stroke is.
class TouchGestures {
  public:
    //: Feed one sample. Returns a gesture on the sample that completes one, `kNone` otherwise.
    //:
    //: `speech_detected` is what keeps a long hold a PTT hold: DEVICE_UI is explicit that speaking
    //: at any point during a hold means the person is talking, not browsing.
    TouchResult feed(const TouchSample& sample, bool speech_detected = false) {
        TouchResult result;

        // A second finger cancels whatever the first was becoming. Not "is ignored": a gesture
        // half-formed when the control gesture began must not complete after it ends, or muting
        // would be followed by a stray tap.
        if (sample.down && sample.fingers > 1) {
            held_ = false;
            reported_hold_ = true;
            taps_ = 0;
            return result;
        }

        if (sample.down && !held_) {
            held_ = true;
            reported_hold_ = false;
            pressed_at_ms_ = sample.at_ms;
            start_x_ = sample.x;
            start_y_ = sample.y;
            travelled_ = 0;
            zone_ = zoneAt(sample.x, sample.y);
            return result;
        }

        if (sample.down && held_ && zone_ == TouchZone::kMicButton) {
            // **A button is a button.** A press that began on the control produces one outcome or
            // none -- never a stroke, never the carousel hold. Sliding off it far enough cancels,
            // which is the ordinary button idiom and the only way to change your mind after
            // touching a control that mutes you.
            const int dx = sample.x - start_x_;
            const int dy = sample.y - start_y_;
            const int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (distance >= kStrokeTravelPx) reported_hold_ = true;  // cancelled
            return result;
        }

        if (sample.down && held_) {
            const int dx = sample.x - start_x_;
            const int dy = sample.y - start_y_;
            const int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (distance > travelled_) travelled_ = distance;

            // A stroke is reported **while it happens**, not on release: the reflex it drives is a
            // contented arc, and a contented arc that arrived after the finger left would be a
            // reaction to a memory.
            if (travelled_ >= kStrokeTravelPx && !reported_hold_) {
                reported_hold_ = true;
                result.gesture = TouchGesture::kStroke;
                result.zone = zoneAt(sample.x, sample.y);
                result.count = 1;
                return result;
            }

            // A hold that passes 1.2 s without speech is the carousel's, and it is reported once.
            if (!reported_hold_ && !speech_detected &&
                sample.at_ms - pressed_at_ms_ >= kCarouselHoldMs) {
                reported_hold_ = true;
                result.gesture = TouchGesture::kHeldSilent;
                result.zone = zone_;
                return result;
            }
            return result;
        }

        if (!sample.down && held_) {
            held_ = false;
            const uint32_t duration = sample.at_ms - pressed_at_ms_;

            if (reported_hold_) return result;  // already spoken for: a stroke, the hold, or a cancel

            if (zone_ == TouchZone::kMicButton) {
                // Control, and it deliberately does **not** join the tap run: `taps_` is affection's
                // counter, and a person reaching for mute has not petted anything.
                //
                // **No duration limit, deliberately.** The first version required the press to be
                // shorter than `kPttHoldMs`, and that was wrong for the same reason it was wrong
                // everywhere else in this file: 120 ms is the threshold at which *push-to-talk*
                // decides a hold is speech, and a deliberate press on a button is 150-250 ms. The
                // button rejected every real press. A button has no second meaning for a long
                // press, so it should accept any press that was not cancelled by sliding off it.
                //
                // The refractory still applies -- a panel that reports one release as two must not
                // toggle twice.
                if (reported_at_ms_ != 0 && sample.at_ms - reported_at_ms_ < kGestureRefractoryMs) {
                    return result;
                }
                reported_at_ms_ = sample.at_ms;
                result.gesture = TouchGesture::kMicToggle;
                result.zone = zone_;
                result.count = 1;
                return result;
            }

            if (duration >= kPttHoldMs) {
                result.gesture = TouchGesture::kLongPress;
                result.zone = zone_;
                return result;
            }

            // A tap. Whether it is *the* tap or part of a run depends on the one before it.
            const bool continues =
                taps_ > 0 && sample.at_ms - last_tap_ms_ <= kMultiTapWindowMs;
            taps_ = continues ? static_cast<uint8_t>(taps_ + 1) : 1;
            last_tap_ms_ = sample.at_ms;

            // **Throttled, not dropped.** `taps_` above already rose; what this suppresses is the
            // report, so a burst produces one reaction every `kGestureRefractoryMs` and each is
            // deeper than the last rather than the same one repeated.
            if (reported_at_ms_ != 0 && sample.at_ms - reported_at_ms_ < kGestureRefractoryMs) {
                return result;
            }
            reported_at_ms_ = sample.at_ms;

            result.zone = zone_;
            result.count = taps_;
            if (zone_ == TouchZone::kEye) {
                // A poke outranks the tap count: poking an eye twice is still poking an eye, and
                // the reaction DEVICE_UI gives it does not accumulate.
                result.gesture = TouchGesture::kPokeEye;
            } else {
                result.gesture = taps_ > 1 ? TouchGesture::kMultiTap : TouchGesture::kTap;
            }
            return result;
        }

        return result;
    }

    //: Drop any run of taps. Called when the face changes state under the finger -- a run that
    //: spanned a reply would report a count nobody performed.
    //:
    //: **It does not abort the press in progress**, and that distinction cost a debugging session.
    //: Clearing `held_` here meant a finger that was down when the state changed could never
    //: complete: the release arrived to a classifier that had forgotten the press, and the gesture
    //: silently evaporated. Since a press *itself* changes the state -- it opens a PTT window --
    //: that made any press longer than 120 ms unclassifiable.
    //:
    //: What the review finding actually asked for is that the **count** not span a state change,
    //: which is `taps_`. The press is a fact about a finger and no state change makes it untrue.
    void reset() {
        taps_ = 0;
        reported_at_ms_ = 0;
    }

    bool isHeld() const { return held_; }

  private:
    bool held_ = false;
    //: Whether this hold has already produced a gesture. A hold reports at most one thing: without
    //: this a stroke would fire on every sample after the travel threshold.
    bool reported_hold_ = false;
    uint32_t pressed_at_ms_ = 0;
    uint32_t last_tap_ms_ = 0;
    int start_x_ = 0;
    int start_y_ = 0;
    int travelled_ = 0;
    uint8_t taps_ = 0;
    //: When a gesture was last reported. Zero means never -- so the first touch after a boot or a
    //: reset is never withheld.
    uint32_t reported_at_ms_ = 0;
    TouchZone zone_ = TouchZone::kOutside;
};

}  // namespace roboface

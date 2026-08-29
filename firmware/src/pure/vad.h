// Voice activity: when did a person start talking, and when did they stop.
//
// **Pure**, and driven by an injected clock, the way `pure/ptt.h` is -- and for the same reason.
// These rules are almost entirely about time, and time is the one thing a device test cannot rush:
// proving "a 90 ms burst is not an utterance" on hardware means making a noise for exactly 90 ms.
//
// **The machine reports transitions, not levels.** `pure/ptt.h` states this rule and this file
// keeps it: a caller polling `isSpeaking()` would have to remember what it last saw, which is the
// same state kept in a second place, and the two would eventually disagree. Every transition here
// fires exactly once per utterance.
//
// **Two measures, not one.** Energy alone calls a slammed door speech. It also calls *rumble*
// speech, which is not hypothetical: v1.3 spent a long evening on captures whose energy sat almost
// entirely below 100 Hz and carried no voice at all -- they measured -26 dBFS and recognised as an
// empty string. Voiced speech crosses zero far more often than a low-frequency thump does, so the
// zero-crossing rate is what separates them, and it costs one comparison per sample.
//
// **A short burst is discarded, not ended.** `pure/ptt.h` refuses to treat a tap as an utterance
// because a window that opened and closed in 80 ms would send `listen_start` and `listen_stop` with
// nothing between them, leaving the server to special-case an empty transcript for something the
// person never said. A cough deserves the same refusal.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/capture.h"
#include "pure/level.h"

namespace roboface {

//: The peak level, 0..1, above which a frame counts as loud. The microphone runs at a fixed
//: magnification and speech at desk distance sits well under half scale, so this is deliberately
//: low; `minimum speech` and the zero-crossing gate are what reject the noise it lets through.
//: **Measured**, not chosen. `/cal 10` in a quiet room and again over ten seconds of ordinary
//: speech at desk distance:
//:
//:            p50   p90   max
//:   silence   9%   13%    23%
//:   speech    9%   23%    99%
//:
//: The threshold sits between the two p90s. Note p50 is identical: half the frames of a person
//: speaking *are* room tone, because speech has gaps -- which is why the duration rules below
//: matter more than this number does.
inline constexpr float kVadSensitivity = 0.17f;

//: Zero crossings per frame below which a loud frame is treated as rumble rather than voice.
//:
//: **Measured, and it discriminates nothing in this room**: silence reached 130 crossings, speech
//: 114 -- the noise floor here is broadband, so it crosses zero at least as often as a voice does.
//: Kept low deliberately: it still rejects the sub-100 Hz rumble it was added for (that case
//: measured a handful of crossings), and it must not reject anything else, because on this
//: evidence it cannot tell speech from a quiet room.
inline constexpr std::size_t kVadMinCrossings = 6;

//: How long speech must last before the utterance is worth sending. Below this it is a cough, a
//: chair, a door -- discarded without ever becoming a turn.
//:
//: Shorter than it looks it should be, for a measured reason: only the loudest fifth of a speaking
//: person's frames clear the threshold, so requiring a long unbroken run of them would reject real
//: speech. What keeps a cough out is this *together with* the hangover below -- a cough is loud for
//: 40 ms and then genuinely over.
inline constexpr uint32_t kVadMinSpeechMs = 150;

//: How long the room must be quiet before the utterance is over. Long enough to survive the pause
//: inside a sentence, short enough that the answer does not feel late.
inline constexpr uint32_t kVadEndPauseMs = 800;

//: How much audio before the trigger is kept, so the first syllable is never lost. Detection needs
//: a few frames to be sure, and those frames are already speech.
inline constexpr uint32_t kVadPreRollMs = 300;

//: Quiet frames tolerated inside speech without restarting the end-pause count. A plosive closes
//: the vocal tract completely -- the gap in "a **p**ple" is real silence, and an endpointer without
//: hangover ends the utterance in the middle of the word.
//: Raised from 120 ms on the same evidence: between two syllables the level drops back to room
//: tone, so a short hangover ends the utterance inside a word. This is what lets an intermittent
//: run of loud frames accumulate into one utterance instead of a string of discarded fragments.
inline constexpr uint32_t kVadHangoverMs = 250;

//: What the endpointer decided about the frame just fed to it.
enum class VadEvent {
    kNone,           // nothing changed
    kSpeechStarted,  // speech has lasted long enough to be real -- open the window
    kSpeechEnded,    // the end-pause elapsed -- close the utterance
    kDiscarded,      // speech stopped before it was long enough -- there was no utterance
};

//: The tunable half, separated so it can be carried, validated and changed at runtime without the
//: endpointer itself knowing where the numbers came from.
struct VadSettings {
    float sensitivity = kVadSensitivity;
    std::size_t min_crossings = kVadMinCrossings;
    uint32_t min_speech_ms = kVadMinSpeechMs;
    uint32_t end_pause_ms = kVadEndPauseMs;
    uint32_t pre_roll_ms = kVadPreRollMs;
    uint32_t hangover_ms = kVadHangoverMs;
};

// How many times a frame's samples cross zero. The measure that separates a voice from a thump:
// both can be loud, only one changes sign often.
//
// A sample of exactly zero is not a crossing and does not reset the sign being tracked -- silence
// padded with zeros would otherwise read as a crossing on the way in and out of every gap.
inline std::size_t zeroCrossings(const int16_t* samples, std::size_t count) {
    if (samples == nullptr || count < 2) return 0;
    std::size_t crossings = 0;
    int last_sign = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int sign = samples[i] > 0 ? 1 : (samples[i] < 0 ? -1 : 0);
        if (sign == 0) continue;
        if (last_sign != 0 && sign != last_sign) ++crossings;
        last_sign = sign;
    }
    return crossings;
}

// Whether one frame carries voice: loud enough, and crossing zero often enough to be a voice
// rather than something heavy.
inline bool frameIsVoiced(const int16_t* samples, std::size_t count, const VadSettings& settings) {
    if (peakLevel(samples, count) < settings.sensitivity) return false;
    return zeroCrossings(samples, count) >= settings.min_crossings;
}

//: The bounds a person may tune within. Wider than anyone should need, narrow enough that a
//: mistyped value is refused rather than accepted into nonsense -- a sensitivity of 0 hears the
//: room's noise floor as continuous speech, and an end-pause of 10 seconds looks like a device
//: that has stopped working.
inline constexpr int kVadMinSensitivityPct = 1;
inline constexpr int kVadMaxSensitivityPct = 90;
inline constexpr uint32_t kVadMinEndPauseMs = 200;
inline constexpr uint32_t kVadMaxEndPauseMs = 3000;

// Set the sensitivity from a percentage of full scale. **Refuses** an out-of-range value rather
// than clamping it: clamping answers "set it to 0" with "done", and the person then spends their
// time wondering why the device behaves as though they had not.
inline bool setSensitivityPct(VadSettings& settings, int percent) {
    if (percent < kVadMinSensitivityPct || percent > kVadMaxSensitivityPct) return false;
    settings.sensitivity = static_cast<float>(percent) / 100.0f;
    return true;
}

// Likewise for the local end-pause backstop.
inline bool setEndPauseMs(VadSettings& settings, long ms) {
    if (ms < static_cast<long>(kVadMinEndPauseMs) || ms > static_cast<long>(kVadMaxEndPauseMs)) {
        return false;
    }
    settings.end_pause_ms = static_cast<uint32_t>(ms);
    return true;
}

// The sensitivity as a percentage, for reporting it back the way it was set.
inline int sensitivityPct(const VadSettings& settings) {
    return static_cast<int>(settings.sensitivity * 100.0f + 0.5f);
}

class Endpointer {
  public:
    Endpointer() = default;
    explicit Endpointer(const VadSettings& settings) : settings_(settings) {}

    const VadSettings& settings() const { return settings_; }
    void configure(const VadSettings& settings) { settings_ = settings; }

    //: True between `kSpeechStarted` and the event that closes the utterance. For the caller's own
    //: bookkeeping -- never as a substitute for the transitions.
    bool inUtterance() const { return state_ == State::kSpeaking; }

    //: Forget everything, without changing the settings. Used when listening resumes after the
    //: device has been speaking: the silence during playback is not part of anyone's pause, and
    //: the tail of the reply is not the start of a sentence.
    void reset() {
        state_ = State::kQuiet;
        voiced_ms_ = 0;
        quiet_ms_ = 0;
    }

    // Feed one frame of PCM. `frame_ms` is how long the frame lasts, which the caller knows exactly
    // -- it is the frame size, not a clock reading -- so the endpointer never needs `millis()` and
    // a host test can run an hour of audio in a millisecond.
    VadEvent feed(const int16_t* samples, std::size_t count, uint32_t frame_ms) {
        return feedVoiced(frameIsVoiced(samples, count, settings_), frame_ms);
    }

    // The same decision from an already-classified frame. Separated so a test can drive the timing
    // rules without synthesising audio, and so a caller that has computed the level for a meter
    // does not pay for it twice.
    VadEvent feedVoiced(bool voiced, uint32_t frame_ms) {
        switch (state_) {
            case State::kQuiet:
                if (!voiced) return VadEvent::kNone;
                state_ = State::kMaybe;
                voiced_ms_ = frame_ms;
                quiet_ms_ = 0;
                // A single loud frame is not yet an utterance. `kMaybe` is what makes the
                // minimum-speech rule possible at all: without it the window would open on the
                // first thump and the rule could only ever end an utterance, never prevent one.
                return maybeStart();

            case State::kMaybe:
                if (voiced) {
                    voiced_ms_ += frame_ms;
                    quiet_ms_ = 0;
                    return maybeStart();
                }
                quiet_ms_ += frame_ms;
                // Still inside the hangover: a gap this short is part of the sound, not the end of
                // it, so the accumulated speech survives.
                if (quiet_ms_ < settings_.hangover_ms) return VadEvent::kNone;
                // It stopped before it was ever long enough. Nothing was opened, so nothing needs
                // closing -- but the caller is told, because "a noise happened and was ignored" is
                // worth counting and is not the same as silence.
                reset();
                return VadEvent::kDiscarded;

            case State::kSpeaking:
                if (voiced) {
                    quiet_ms_ = 0;
                    return VadEvent::kNone;
                }
                quiet_ms_ += frame_ms;
                if (quiet_ms_ < settings_.end_pause_ms) return VadEvent::kNone;
                reset();
                return VadEvent::kSpeechEnded;
        }
        return VadEvent::kNone;
    }

  private:
    enum class State {
        kQuiet,     // nothing heard
        kMaybe,     // something is being said, not yet long enough to believe
        kSpeaking,  // the window is open
    };

    VadEvent maybeStart() {
        if (voiced_ms_ < settings_.min_speech_ms) return VadEvent::kNone;
        state_ = State::kSpeaking;
        quiet_ms_ = 0;
        return VadEvent::kSpeechStarted;
    }

    VadSettings settings_{};
    State state_ = State::kQuiet;
    uint32_t voiced_ms_ = 0;
    uint32_t quiet_ms_ = 0;
};

// How many frames of pre-roll a setting asks for. The ring that holds them is glue (it owns
// buffers); how many it must hold is arithmetic, and belongs here where a host can check it.
inline constexpr std::size_t preRollFrames(uint32_t pre_roll_ms, int frame_ms = kCaptureFrameMs) {
    if (pre_roll_ms == 0 || frame_ms <= 0) return 0;
    return (pre_roll_ms + static_cast<uint32_t>(frame_ms) - 1u) / static_cast<uint32_t>(frame_ms);
}

}  // namespace roboface

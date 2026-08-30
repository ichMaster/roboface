// Streaming playback: PCM arriving over the socket comes out of the speaker as it arrives.
//
// **`M5.Speaker.playRaw` does not wait for a free slot — it returns false and drops the chunk.**
// (`Speaker_Class::_set_next_wav` returns false when both slots on the channel are claimed.) The
// network delivers a reply far faster than 16 kHz real time, so the slots fill within about a
// second and every chunk after that is discarded. The audible symptom is precise: the reply starts
// cleanly and then breaks up.
//
// So the backlog is held in a **PSRAM ring** and fed to the speaker only as fast as it accepts —
// `playRaw`'s return value is the pacing signal, and a chunk it refuses is retried rather than
// lost. A fifteen-second reply is about 480 KB of PCM, which is why the ring cannot live in
// internal RAM.
//
// Two further hazards, both real on this stack:
//
// * **`playRaw` keeps the pointer, it does not copy** (`info.data = data`). The I2S task reads it
//   after the call returns, so each chunk is copied into a pool slot that outlives the call.
// * **Channel −1 means "any free channel"**, so consecutive chunks land on different channels and
//   sound *together*. That is a chord, not a queue. The channel is pinned.
//
// The Core S3 shares one I2S bus between microphone and speaker, so it is claimed once per turn
// and given back on `tts_end`.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pure/capture.h"
#include "pure/pre_roll.h"
#include "pure/envelope.h"
#include "pure/level.h"
#include "pure/pcm_ring.h"
#include "pure/stereo.h"

namespace app {

//: About sixteen seconds of 16 kHz PCM16, in PSRAM. Sized for a whole reply so the socket is
//: rarely throttled; when it is, the ring reports it and `main` stops pumping, which turns the
//: overflow into TCP backpressure instead of lost speech.
inline constexpr std::size_t kBacklogBytes = 256 * 1024;

//: Stop reading the socket once free space drops below this.
inline constexpr std::size_t kBackpressureMargin = 16 * 1024;

//: Used when PSRAM cannot provide the full backlog. Small, but the pacing does not depend on the
//: size -- a shorter buffer only means the socket is throttled sooner.
inline constexpr std::size_t kFallbackBacklogBytes = 48 * 1024;

//: Bytes handed to the speaker per queued buffer. ~32 ms at 16 kHz: short enough that `tts_end`
//: is acted on promptly, long enough that queueing is not the dominant cost.
inline constexpr std::size_t kChunkBytes = 1024;

//: How much of a reply must be in hand before the speaker is allowed to start. At 32 kB/s this is
//: a quarter of a second of cushion.
//:
//: Playback used to begin on the first byte that arrived, which sounds like the fastest possible
//: answer and is in fact the choppiest: the speaker drains the handful of bytes it was given and
//: then waits, so the reply arrives in audible pieces. Synthesis is per phrase and the network is
//: not smooth, so some cushion is the difference between a voice and a stutter.
//:
//: It costs a quarter second of time-to-first-audio, which is real and worth stating -- but a
//: reply that is heard late is better than one that is heard broken.
inline constexpr std::size_t kPlaybackPrimeBytes = 8 * 1024;

//: Buffers in flight. `playRaw` queues two per channel, so three is one more than can be pending
//: and a refill never lands on memory the I2S task is still reading.
inline constexpr std::size_t kChunkSlots = 3;

//: How long after the last queued buffer the bus is given back regardless. `isPlaying(channel)`
//: counts *occupied slots*, and a slot can stay claimed after its audio has finished playing --
//: waiting on it alone left the speaker enabled forever, which would deny v1.2's microphone the
//: bus it needs. The timeout is generous: it only has to outlast one queued buffer (~32 ms).
inline constexpr uint32_t kDrainTimeoutMs = 1500;

class AudioIo {
  public:
    //: Called with one captured frame, the moment it fills. Returning false means the caller could
    //: not send it -- capture keeps going rather than stalling, because the microphone does not
    //: pause for the network and a gap is better than a stall that loses the rest of the sentence.
    using FrameSink = bool (*)(const uint8_t* data, std::size_t length);

    // Allocates the PSRAM backlog. Returns false if it could not -- a renderer that failed to
    // allocate must say so rather than presenting a mute device as a working one.
    bool begin(uint8_t volume, uint8_t mic_gain = 16);

    // Take the bus for playback. Idempotent: the second chunk of a turn must not re-switch.
    void startSpeaking();

    // Buffer one `tts_audio` payload. Returns what was accepted; a short return is backpressure.
    std::size_t write(const uint8_t* data, std::size_t length);

    // Feed the speaker as fast as it will take. Call every loop.
    void tick(uint32_t now_ms);

    // --- capture (v1.2) ---------------------------------------------------------------
    //
    // The mirror of playback: the Core S3 shares one I2S bus, so taking it for the microphone
    // ends the speaker exactly as taking it for the speaker ends the microphone. The two must
    // never both hold it, which is why both paths go through this one class.

    // Take the bus for capture and begin. Idempotent.
    //: Keep the microphone running with no window open. Step 1 of active listening: the frames
    //: are discarded, and the only question this answers is whether an always-on recorder still
    //: captures at full rate.
    //: Called for every captured frame, window or not. Step 2: the VAD sees the room without
    //: being allowed to act on it.
    using FrameObserver = void (*)(const int16_t* samples, std::size_t count, uint32_t frame_ms);
    bool startMonitoring(FrameObserver observer = nullptr);
    //: Stop hearing the room. The microphone is released only if no window is open on top of it.
    void stopMonitoring();
    bool isMonitoring() const { return monitoring_; }

    //: Send everything captured before the window opened, oldest first. Detection needs a few
    //: frames to be sure and those frames are already words: without this every utterance reaches
    //: the recogniser with its first syllable missing.
    std::size_t flushPreRoll();

    //: Consumed by reading: the endpointer is cleared once when hearing resumes, not every tick.
    bool takeMonitorResumed() {
        const bool resumed = monitor_resumed_;
        monitor_resumed_ = false;
        return resumed;
    }

    bool startListening(FrameSink sink);

    // Stop capturing and release the bus. Safe when not listening.
    void stopListening();

    // Capture straight into the playback backlog, for the loopback diagnostic. Returns what was
    // stored; a short return means the backlog is full, which for a fixed-length recording means
    // the recording is over.
    std::size_t captureToBacklog(const uint8_t* data, std::size_t length) {
        return backlog_.write(data, length);
    }

    // Play what the backlog holds, without clearing it first -- the mirror of `startSpeaking` for
    // audio that is already buffered rather than arriving.
    void playBacklog();

    bool isListening() const { return listening_; }
    const roboface::CaptureTally& tally() const { return tally_; }

    //: 0..1, from the frame just captured. The meter reads this rather than the raw samples: the
    //: envelope is arithmetic and belongs in `pure/`, and the drawing should not be handed a
    //: buffer it might outlive.
    float inputLevel() const { return level_; }

    //: 0..1, the loudness of what the speaker is playing **right now** -- the signal the mouth
    //: moves on.
    //:
    //: A different measurement from `inputLevel`, and the distinction is the whole reason lip-sync
    //: is not simply the level meter: that one is the microphone, this one is the speaker. Wiring
    //: the mouth to the microphone would make the face mime the person talking to it.
    //:
    //: Taken from the chunk being handed to the speaker, so it leads the sound by however much the
    //: speaker has buffered -- about two chunks, ~60 ms. Close enough that mouth and voice read as
    //: one thing; v2.3's real lip-sync accounts for it properly.
    float outputLevel() const { return output_level_; }

    //: The loudest frame of the current capture. A capture that reports zero here recorded
    //: silence, which is a different fault from a playback that fails to make a sound -- and the
    //: two are indistinguishable by ear.
    float peakSeen() const { return peak_seen_; }

    // `tts_end`: play out the backlog, then give the bus back.
    void finish();

    // `restart` or an error: stop now and discard. Speech that has been superseded is worse than
    // silence -- it answers a question nobody is still asking.
    void abort();

    bool isSpeaking() const { return speaking_; }

    //: A short click, for a **control** gesture the person cannot otherwise tell landed.
    //:
    //: Goes through here rather than calling `M5.Speaker` directly, because the speaker shares one
    //: I2S bus with the microphone and this class owns that. A tone played around it would meet a
    //: port still configured for capture -- `Speaker.begin()` reports success and produces silence,
    //: which is the most misleading pair of symptoms in this subsystem and cost v1 an evening.
    //:
    //: **Refused while the device is speaking.** A reply must never be interrupted by feedback
    //: about a button, and the reflex layer follows the same rule for the same reason.
    void click();
    //: Only while actually speaking, and only with a backlog to fill. An unattached ring reports
    //: zero free, so an unguarded version throttled the socket **permanently** -- the device never
    //: reached the server at all, which looks like a network fault rather than an audio one.
    bool isBackpressured() const {
        return speaking_ && backlog_.attached() && backlog_.free() < kBackpressureMargin;
    }
    std::size_t buffered() const { return backlog_.size(); }
    std::size_t backlogCapacity() const { return backlog_.capacity(); }
    uint32_t bytesQueued() const { return bytes_queued_; }
    uint32_t chunksRefused() const { return chunks_refused_; }
    //: Both channels of a completed frame, deinterleaved. Separate from `FrameObserver`, which is
    //: the VAD's and takes one channel: the VAD does not want two, and the direction estimator
    //: cannot work with one. Widening the existing callback would have made every caller carry a
    //: parameter it ignores.
    using StereoObserver = void (*)(const int16_t* left, const int16_t* right, std::size_t count,
                                    uint32_t at_ms);
    void onStereoFrame(StereoObserver observer) { stereo_observer_ = observer; }

    //: What the two channels last measured, and the widest imbalance since boot. `/mic-levels`.
    const roboface::StereoLevels& stereoLevels() const { return levels_; }
    //: Which microphones the last uplink frame was made from. `/mic-levels` reports it: a frame
    //: built from one channel is a fact worth being able to see, since the symptom of a covered
    //: microphone is otherwise just "it hears me a bit worse".
    roboface::MonoSource monoSource() const { return mono_source_; }
    float balanceMin() const { return balance_min_; }
    float balanceMax() const { return balance_max_; }
    void resetBalanceRange() {
        balance_min_ = 0.0f;
        balance_max_ = 0.0f;
    }

    //: Audio that had nowhere to go. Non-zero means the backlog never allocated, and the device
    //: is mute for a reason it can state rather than for no visible reason at all.
    uint32_t bytesDropped() const { return bytes_dropped_; }

  private:
    void releaseBus();

    roboface::PcmRing backlog_;

    //: Two capture frames, alternating: the microphone records into one while the other is being
    //: sent. A single buffer would either drop the samples arriving during the send or make the
    //: send wait for the next frame, and both show up as gaps in the middle of words.
    //:
    //: **Interleaved stereo since v2.5** -- twice the samples for the same 20 ms. The duration is
    //: what stays constant; a version of this that kept the sample count would silently halve every
    //: timing downstream and still look reasonable.
    int16_t capture_[2][roboface::kStereoFrameSamples] = {};

    //: Where a frame goes after it stops being interleaved. Members rather than locals because they
    //: are filled every 20 ms and the stack in the audio path is not the place to find that out.
    int16_t left_[roboface::kCaptureFrameSamples] = {};
    int16_t right_[roboface::kCaptureFrameSamples] = {};
    roboface::StereoLevels levels_;
    //: What the uplink actually carries: both channels averaged, or the live one when the other is
    //: obstructed. Its own buffer rather than mixing in place, because `left_` is still wanted --
    //: the direction estimate reads both channels after the mix has been made.
    int16_t mono_[roboface::kCaptureFrameSamples] = {};
    roboface::SourceChooser source_chooser_;
    roboface::MonoSource mono_source_ = roboface::MonoSource::kBoth;

    //: The furthest the balance has swung each way since boot. **Signed extremes, not a magnitude**
    //: -- "some imbalance happened" is a much weaker claim than "it went both ways", and only the
    //: second one distinguishes two microphones from one noisy channel. The person making a noise on
    //: one side of the board and the person reading serial are not the same person, so the number
    //: has to survive until someone can look at it: the same reason `/touch` keeps a ring.
    float balance_min_ = 0.0f;
    float balance_max_ = 0.0f;
    std::size_t capture_slot_ = 0;
    bool listening_ = false;
    bool monitoring_ = false;
    FrameObserver observer_ = nullptr;
    StereoObserver stereo_observer_ = nullptr;
    //: Whether the room should be monitored at all, independent of whether the bus is momentarily
    //: busy. Playback clears `monitoring_`; this is what says to put it back afterwards.
    bool monitor_wanted_ = false;

    //: The last few frames captured with no window open. 10 frames is 200 ms -- enough for the
    //: syllable detection spends confirming itself, and 6.4 KB of the internal RAM the I2S DMA
    //: buffers also come out of, which is why it is not larger.
    static constexpr std::size_t kPreRollFrames = 10;
    int16_t pre_roll_[kPreRollFrames][roboface::kCaptureFrameSamples] = {};
    roboface::PreRollRing pre_roll_slots_{kPreRollFrames, kPreRollFrames};
    //: True once, on the tick where listening resumed -- the caller's cue to clear its endpointer.
    bool monitor_resumed_ = false;
    FrameSink sink_ = nullptr;
    roboface::CaptureTally tally_;
    float level_ = 0.0f;
    float output_level_ = 0.0f;
    //: When the envelope last moved. What makes its decay a function of elapsed time rather than
    //: of chunks arriving -- see the note at the decay in `tick`.
    uint32_t level_updated_ms_ = 0;
    float peak_seen_ = 0.0f;
    uint8_t pool_[kChunkSlots][kChunkBytes] = {};
    std::size_t slot_ = 0;
    bool speaking_ = false;
    bool draining_ = false;
    //: Whether enough of the reply has arrived to start playing it out.
    bool primed_ = false;
    uint8_t volume_ = 120;
    uint8_t mic_gain_ = 16;
    uint32_t bytes_queued_ = 0;
    uint32_t last_queued_ms_ = 0;
    //: How often the speaker refused a buffer. Not a fault -- it is the pacing working -- but a
    //: count of zero across a long reply would mean the ring is not being exercised at all.
    uint32_t chunks_refused_ = 0;
    uint32_t bytes_dropped_ = 0;
};

}  // namespace app

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
#include "pure/half_duplex.h"
#include "pure/pre_roll.h"
#include "pure/vad.h"
#include "pure/level.h"
#include "pure/pcm_ring.h"

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

    //: Called for **every** captured frame, whether or not a window is open. This is what lets the
    //: VAD hear the room before anyone has asked it to listen.
    using FrameObserver = void (*)(const int16_t* samples, std::size_t count, uint32_t frame_ms);

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
    //: Capture with no window open: frames reach the observer and the pre-roll ring, and go
    //: nowhere else. Active listening needs the microphone running *before* there is anything to
    //: send, because the decision to send is made from what it hears.
    bool startMonitoring(FrameObserver observer);
    void stopMonitoring();
    bool isMonitoring() const { return monitoring_; }

    //: True once, on the loop where listening resumed after playback -- the caller's cue to clear
    //: its endpointer. Consumed by reading it, so the reset happens once and not on every loop.
    bool takeMonitorResumed() {
        const bool resumed = monitor_resumed_;
        monitor_resumed_ = false;
        return resumed;
    }

    //: Send everything the pre-roll ring holds through `sink`, oldest first. Returns the frames
    //: sent. Detection takes a few frames to be sure, and those frames are already speech -- so
    //: without this the utterance reaches the recogniser with its first syllable missing.
    std::size_t flushPreRoll(FrameSink sink);

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
    //: Audio that had nowhere to go. Non-zero means the backlog never allocated, and the device
    //: is mute for a reason it can state rather than for no visible reason at all.
    uint32_t bytesDropped() const { return bytes_dropped_; }

  private:
    void releaseBus();

    //: Claim the shared bus for input and arm both queue slots. Shared by monitoring and by a
    //: window opened without monitoring, so the two cannot drift apart.
    bool beginCapture();

    //: Read one completed frame, if the recorder has finished one. The ordering inside is load
    //: bearing and was arrived at the hard way -- do not reorder it.
    void drainCapturedFrame();

    roboface::PcmRing backlog_;

    //: Two capture frames, alternating: the microphone records into one while the other is being
    //: sent. A single buffer would either drop the samples arriving during the send or make the
    //: send wait for the next frame, and both show up as gaps in the middle of words.
    int16_t capture_[2][roboface::kCaptureFrameSamples] = {};
    std::size_t capture_slot_ = 0;
    bool listening_ = false;
    bool monitoring_ = false;
    //: Whether the application wants the room monitored at all, independent of whether the bus is
    //: momentarily busy. Playback suspends `monitoring_`; this is what says to put it back.
    roboface::HalfDuplexGuard duplex_;
    bool monitor_resumed_ = false;
    FrameSink sink_ = nullptr;
    FrameObserver observer_ = nullptr;

    //: The pre-roll ring, sized at compile time for the longest pre-roll the settings may ask for.
    //: 15 frames of 640 bytes is ~9.6 KB of internal RAM -- worth stating, because internal RAM is
    //: also where the I2S DMA buffers come from and there is not much of it left (see `/mem`).
    static constexpr std::size_t kPreRollFrames = roboface::preRollFrames(roboface::kVadPreRollMs);
    int16_t pre_roll_[kPreRollFrames][roboface::kCaptureFrameSamples] = {};
    //: Where each frame goes and what order they come back out -- pure, and host-tested, because
    //: the failure it prevents is replaying audio in the wrong order rather than losing it.
    roboface::PreRollRing pre_roll_slots_{kPreRollFrames, kPreRollFrames};
    roboface::CaptureTally tally_;
    float level_ = 0.0f;
    float peak_seen_ = 0.0f;
    uint8_t pool_[kChunkSlots][kChunkBytes] = {};
    std::size_t slot_ = 0;
    bool speaking_ = false;
    bool draining_ = false;
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

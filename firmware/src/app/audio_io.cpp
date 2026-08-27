#include "app/audio_io.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace app {
namespace {

//: The device's playback format, and the server's: `AUDIO_FMT` in `protocol.py` is
//: `pcm16/16000/1`, and ElevenLabs is asked for `pcm_16000`, so nothing decodes anywhere.
constexpr uint32_t kSampleRate = 16000;

//: Pinned, so successive buffers queue behind each other instead of sounding together.
constexpr int kChannel = 0;

}  // namespace

bool AudioIo::begin(uint8_t volume, uint8_t mic_gain) {
    volume_ = volume;
    mic_gain_ = mic_gain;

    // PSRAM first: a whole reply is hundreds of kilobytes and internal RAM is wanted by the
    // network stack and the sprite. Falling back rather than refusing, because a smaller backlog
    // still works -- the socket is throttled sooner, and TCP holds the rest on the server.
    std::size_t wanted = kBacklogBytes;
    auto* storage = static_cast<uint8_t*>(heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM));
    if (storage == nullptr) {
        wanted = kFallbackBacklogBytes;
        storage = static_cast<uint8_t*>(heap_caps_malloc(wanted, MALLOC_CAP_INTERNAL));
    }
    if (storage == nullptr) return false;

    backlog_.attach(storage, wanted);

    // The capture ring. Internal RAM on purpose: the recorder writes it by DMA every 20 ms, and it
    // is small enough that the sprite and the network stack do not miss it.
    ring_ = static_cast<int16_t*>(
        heap_caps_malloc(kCaptureRingSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    if (ring_ == nullptr) return false;

    return enterMicMode();
}

bool AudioIo::enterMicMode() {
    // Input is this board's **resting** state, and it is left running between windows rather than
    // torn down after each one. Stopping and restarting the codec around every utterance is what
    // the two-peripherals-one-bus shape seems to ask for, and it is where the capture faults came
    // from: a restart that reports success while the port is still held for output, or a config
    // applied to a driver that reads it back at the wrong moment.
    //
    // So: release the speaker if it holds the bus, and start the microphone only if it is not
    // already running. Both guarded, because the no-op case is the common one.
    if (M5.Speaker.isEnabled()) M5.Speaker.end();
    if (M5.Mic.isEnabled()) return true;

    // The library's default magnification of 16 leaves this board's ES7210 reading about 1% of
    // full scale for speech at desk distance -- audible to nothing. Set before `begin`, because
    // the config is read when the driver starts.
    auto mic_cfg = M5.Mic.config();
    mic_cfg.magnification = mic_gain_;
    M5.Mic.config(mic_cfg);
    return M5.Mic.begin();
}

void AudioIo::startSpeaking() {
    if (speaking_) return;  // the bus is already ours; switching again would glitch the output
    // One bus, two peripherals. End the microphone first so I2S is not torn down under a live
    // capture -- v1.2's capture path will do the reverse.
    if (M5.Mic.isEnabled()) M5.Mic.end();
    if (!M5.Speaker.begin()) return;
    M5.Speaker.setVolume(volume_);
    speaking_ = true;
    draining_ = false;
}

std::size_t AudioIo::write(const uint8_t* data, std::size_t length) {
    if (!backlog_.attached()) {
        // Zero, not `length`. Returning `length` said "I accepted all of it" when nothing had,
        // and that is precisely how this phase's hardest bug hid: PSRAM reported zero free, the
        // ring never attached, and the device sat silent while the server logged hundreds of
        // chunks sent and the board's own counters read q=0. Every layer had been told the audio
        // was fine. Counting the loss makes a device that cannot allocate say so continuously
        // rather than once at boot.
        bytes_dropped_ += static_cast<uint32_t>(length);
        return 0;
    }
    return backlog_.write(data, length);
}

bool AudioIo::startListening(FrameSink sink) {
    if (listening_) return true;
    // One bus, two peripherals -- the mirror of `startSpeaking`. Anything the speaker still holds
    // is abandoned: a person pressing to talk has superseded whatever was being said to them.
    if (speaking_) abort();
    if (!enterMicMode()) return false;

    sink_ = sink;
    tally_.reset();
    peak_seen_ = 0.0f;
    timing_.start(millis());
    listening_ = true;

    // Fill the recorder's queue before returning. It is two deep, and arming one region at a time
    // leaves it idle from the moment a region completes until `tick` notices -- a gap in every
    // frame. Measured, that cost a quarter of the audio: a three-second window produced 2260 ms.
    topUpQueue();
    return true;
}

void AudioIo::stopListening() {
    if (!listening_) return;
    listening_ = false;
    sink_ = nullptr;
    level_ = 0.0f;  // the meter must not hold the last word's level after the window closes
    // The microphone is **left running**: capture is this board's resting state, and stopping the
    // codec here only to start it again for the next window is the restart churn the capture
    // faults came out of. `startSpeaking` releases it when the bus is genuinely needed for output.
}

void AudioIo::playBacklog() {
    // Deliberately does not clear: `startSpeaking` releases the microphone and claims the speaker,
    // and the audio to play is already in the ring. Clearing would discard the recording this
    // exists to hear.
    if (listening_) stopListening();
    if (speaking_) return;
    // Tear the port down before claiming it for output. `Mic.end()` alone leaves the shared I2S
    // configured for capture on this board, and `Speaker.begin()` then reports success while
    // producing nothing -- capture reads a healthy 72% peak and the speaker is silent, which is
    // the most misleading pair of symptoms in the whole subsystem.
    if (M5.Mic.isEnabled()) M5.Mic.end();
    M5.Speaker.end();
    delay(20);
    if (!M5.Speaker.begin()) return;
    M5.Speaker.setVolume(volume_);
    speaking_ = true;
    draining_ = true;  // there is no `tts_end` coming; play out and release
}

void AudioIo::topUpQueue() {
    // Hand the recorder whole frames of the ring, in order, while it will take them and while the
    // reader is far enough ahead that a new region cannot lap unsent audio. Regions never straddle
    // the wrap: the ring is a whole number of frames long.
    while (M5.Mic.isRecording() < 2 &&
           timing_.pending() + roboface::kCaptureFrameSamples <= kCaptureRingSamples) {
        const std::size_t offset = timing_.queuedSamples() % kCaptureRingSamples;
        if (!M5.Mic.record(&ring_[offset], roboface::kCaptureFrameSamples,
                           roboface::kCaptureSampleRate)) {
            return;
        }
        timing_.queued(roboface::kCaptureFrameSamples);
    }
}

void AudioIo::tick(uint32_t now_ms) {
    if (listening_) {
        // A frame is ready when the recorder has stopped filling it. Sending happens here, in the
        // loop, rather than in an interrupt: the socket write can block, and blocking inside the
        // I2S callback would drop the samples arriving behind it.
        // Keep the recorder busy, then read back only what the clock says it has finished. The
        // queue depth is *not* used to decide what is readable: `record` returning true means the
        // request was accepted, not that the samples exist, and reading on that signal hands the
        // sink memory the DMA is still writing into.
        topUpQueue();

        // **Bounded.** A late loop can leave several frames confirmed at once, and draining them
        // all in one tick hands the socket a burst it answers with EAGAIN -- the write fails, the
        // frames are lost, and the link spends the rest of the window recovering. Catching up two
        // frames a tick still outruns the recorder's one per 20 ms without ever bursting.
        std::size_t drained = 0;
        while (drained < kMaxFramesPerTick &&
               timing_.readable(now_ms, roboface::kCaptureFrameSamples)) {
            ++drained;
            const std::size_t offset = timing_.sentSamples() % kCaptureRingSamples;
            const int16_t* frame = &ring_[offset];
            timing_.sent(roboface::kCaptureFrameSamples);

            const float peak = roboface::peakLevel(frame, roboface::kCaptureFrameSamples);
            if (peak > peak_seen_) peak_seen_ = peak;
            level_ = roboface::decayToward(level_, peak);

            if (sink_ != nullptr && sink_(reinterpret_cast<const uint8_t*>(frame),
                                          roboface::kCaptureFrameBytes)) {
                tally_.recordFrame(roboface::kCaptureFrameBytes);
            }
        }
    }

    if (!speaking_) return;

    // Hand over buffers until the speaker refuses one. `playRaw` returning false is the whole
    // pacing mechanism: it means both slots are claimed, and the chunk stays in the ring for the
    // next tick rather than being thrown away.
    // **Bounded by what the speaker is actually holding**, not by what the backlog has. `playRaw`
    // returns true even when both slots on the channel are occupied, so an unbounded loop hands it
    // the whole backlog at once and then recycles the three pool buffers underneath audio the I2S
    // task has not read yet. TTS never showed this because its chunks arrive network-paced; a
    // loopback recording is already buffered, drains in one tick, and plays as silence.
    while (!backlog_.empty() && M5.Speaker.isPlaying(kChannel) < 2) {
        uint8_t* buffer = pool_[slot_];
        const std::size_t got = backlog_.readSamples(buffer, kChunkBytes);
        if (got == 0) break;

        if (!M5.Speaker.playRaw(reinterpret_cast<const int16_t*>(buffer), got / sizeof(int16_t),
                                kSampleRate, /*stereo=*/false, /*repeat=*/1, kChannel,
                                /*stop_current_sound=*/false)) {
            // Refused. Put it back at the *front* -- writing it to the tail would reorder the
            // reply, which is worse than a pause.
            ++chunks_refused_;
            backlog_.unread(got);
            break;
        }
        slot_ = slot_ + 1 == kChunkSlots ? 0 : slot_ + 1;
        bytes_queued_ += static_cast<uint32_t>(got);
        last_queued_ms_ = now_ms;
    }

    // Release only once the backlog is drained *and* the speaker has gone quiet -- releasing in
    // `finish()` would cut off whatever was still queued, which is the last part of the last word.
    //
    // The deadline is the safety net: `isPlaying(channel)` counts occupied slots, and a slot can
    // remain claimed after its audio has finished, so waiting on it alone left the speaker enabled
    // for the rest of the session and would have denied v1.2's microphone the shared bus.
    if (!draining_ || !backlog_.empty()) return;
    const bool quiet = !M5.Speaker.isPlaying();
    const bool overdue = now_ms - last_queued_ms_ > kDrainTimeoutMs;
    if (quiet || overdue) releaseBus();
}

void AudioIo::finish() {
    if (!speaking_) return;
    draining_ = true;
}

void AudioIo::abort() {
    if (!speaking_) {
        backlog_.clear();
        return;
    }
    M5.Speaker.stop(kChannel);
    backlog_.clear();
    releaseBus();
}

void AudioIo::releaseBus() {
    M5.Speaker.end();
    speaking_ = false;
    draining_ = false;
    backlog_.clear();
}

}  // namespace app

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
    return true;
}

void AudioIo::startSpeaking() {
    if (speaking_) return;  // the bus is already ours; switching again would glitch the output
    // One bus, two peripherals. End the microphone first so I2S is not torn down under a live
    // capture -- v1.2's capture path does the reverse.
    //
    // Monitoring stops for the duration: the bus is about to belong to the speaker, and a `tick`
    // that kept draining would be reading a recorder that no longer exists. RF-050 owns putting it
    // back afterwards; what matters here is that the pre-roll does not survive, or the first thing
    // the device hears after speaking would be the end of its own sentence.
    duplex_.playbackStarted();
    monitoring_ = false;
    pre_roll_slots_.clear();
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

bool AudioIo::beginCapture() {
    // One bus, two peripherals -- the mirror of `startSpeaking`. Anything the speaker still holds
    // is abandoned: a person talking has superseded whatever was being said to them.
    if (speaking_) abort();
    if (M5.Speaker.isEnabled()) M5.Speaker.end();

    // The library's default magnification of 16 leaves this board's ES7210 reading about 1% of
    // full scale for speech at desk distance -- audible to nothing. Set before `begin`, because
    // the config is read when the driver starts.
    auto mic_cfg = M5.Mic.config();
    mic_cfg.magnification = mic_gain_;
    M5.Mic.config(mic_cfg);
    if (!M5.Mic.begin()) return false;

    capture_slot_ = 0;

    // **Both** slots armed. The microphone has a two-deep queue (`isRecording()` returns 0, 1 or
    // 2), and arming one at a time leaves it idle from the moment a frame completes until `tick`
    // notices and re-arms -- a gap in every frame. Measured, that cost a quarter of the audio: a
    // three-second window produced 2260 ms. With both armed the recorder never stops, and the
    // loop's job is only to drain and re-arm the slot that freed.
    M5.Mic.record(capture_[0], roboface::kCaptureFrameSamples, roboface::kCaptureSampleRate);
    M5.Mic.record(capture_[1], roboface::kCaptureFrameSamples, roboface::kCaptureSampleRate);
    return true;
}

bool AudioIo::startMonitoring(FrameObserver observer) {
    observer_ = observer;
    duplex_.wantListening(true);
    if (monitoring_ || listening_) return true;
    if (!beginCapture()) return false;
    pre_roll_slots_.clear();
    monitoring_ = true;
    return true;
}

void AudioIo::stopMonitoring() {
    duplex_.wantListening(false);
    monitoring_ = false;
    pre_roll_slots_.clear();
    // The microphone is released only if no window is open on top of it -- `stopListening` owns
    // that case, and tearing the codec down under a live capture is exactly the churn that made
    // capture unreliable before.
    if (!listening_ && M5.Mic.isEnabled()) M5.Mic.end();
}

std::size_t AudioIo::flushPreRoll(FrameSink sink) {
    if (sink == nullptr) return 0;
    const std::size_t held = pre_roll_slots_.held();
    std::size_t sent = 0;
    for (std::size_t i = 0; i < held; ++i) {  // oldest first -- the order the audio happened in
        const std::size_t slot = pre_roll_slots_.readSlot(i);
        if (slot >= kPreRollFrames) break;
        if (!sink(reinterpret_cast<const uint8_t*>(pre_roll_[slot]), roboface::kCaptureFrameBytes)) {
            break;
        }
        tally_.recordFrame(roboface::kCaptureFrameBytes);
        ++sent;
    }
    // Consumed either way: these frames are now part of the utterance, and replaying them into a
    // later one would put the same audio on the wire twice.
    pre_roll_slots_.clear();
    return sent;
}

bool AudioIo::startListening(FrameSink sink) {
    if (listening_) return true;
    // Already monitoring means the microphone is running and the pre-roll is full. Opening the
    // window must not restart the codec: the frames arriving right now are the first syllable.
    if (!monitoring_ && !beginCapture()) return false;

    sink_ = sink;
    tally_.reset();
    peak_seen_ = 0.0f;
    listening_ = true;
    return true;
}

void AudioIo::stopListening() {
    if (!listening_) return;
    listening_ = false;
    sink_ = nullptr;
    level_ = 0.0f;  // the meter must not hold the last word's level after the window closes
    // The microphone stays on if the room is still being monitored: closing a window is not a
    // reason to stop hearing, and restarting the codec around every utterance is the churn that
    // made capture unreliable. With no monitor there is nothing listening, so release the bus.
    if (!monitoring_ && M5.Mic.isEnabled()) M5.Mic.end();
    if (monitoring_) pre_roll_slots_.clear();
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

void AudioIo::drainCapturedFrame() {
    // A frame is ready when the recorder has stopped filling it. Sending happens here, in the
    // loop, rather than in an interrupt: the socket write can block, and blocking inside the
    // I2S callback would drop the samples arriving behind it.
    // Fewer than two queued means one has completed, and the slots complete in the order they
    // were armed -- so the one that freed is `capture_slot_`.
    if (M5.Mic.isRecording() >= 2) return;

    const auto* frame = reinterpret_cast<const uint8_t*>(capture_[capture_slot_]);
    const std::size_t completed = capture_slot_;
    capture_slot_ = capture_slot_ == 0 ? 1 : 0;

    // **Everything that reads this buffer happens before it is handed back**, because `record`
    // gives it to the DMA immediately and the recorder then owns it. The peak, the endpointer's
    // scan of all 320 samples, and the copy into the pre-roll are microseconds of local work; done
    // after the re-arm they read a blend of two moments, and the pre-roll that results sounds
    // nearly right -- the failure mode that cost an evening in v1.3, where audio had a plausible
    // level, a plausible frame count and no intelligible speech in it.
    const float peak = roboface::peakLevel(capture_[completed], roboface::kCaptureFrameSamples);
    if (peak > peak_seen_) peak_seen_ = peak;
    level_ = roboface::decayToward(level_, peak);

    // The observer sees every frame, window or not -- it is what decides whether there should be
    // a window at all.
    if (observer_ != nullptr) {
        observer_(capture_[completed], roboface::kCaptureFrameSamples, roboface::kCaptureFrameMs);
    }

    // No window: keep the frame in case one opens in a moment. How many are kept is the ring's
    // business -- a shorter pre-roll setting caps it without the storage changing.
    if (!listening_) {
        const std::size_t slot = pre_roll_slots_.writeSlot();
        if (slot < kPreRollFrames) {
            for (std::size_t i = 0; i < roboface::kCaptureFrameSamples; ++i) {
                pre_roll_[slot][i] = capture_[completed][i];
            }
        }
    }

    // Re-armed once the reading is done, and **before the send** -- which is the only slow step
    // here and the one the queue depth actually matters for. A socket write is milliseconds; the
    // recorder must not sit one deep for that long.
    M5.Mic.record(capture_[completed], roboface::kCaptureFrameSamples,
                  roboface::kCaptureSampleRate);

    if (listening_ && sink_ != nullptr && sink_(frame, roboface::kCaptureFrameBytes)) {
        tally_.recordFrame(roboface::kCaptureFrameBytes);
    }
}


void AudioIo::tick(uint32_t now_ms) {
    if (listening_ || monitoring_) drainCapturedFrame();

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

    // **Every** route out of playback passes through here -- drained, cancelled, aborted, faulted.
    // That is why listening is resumed here and nowhere else: a reply that ended by a path nobody
    // anticipated would otherwise leave the device permanently deaf while looking perfectly
    // healthy -- connected, idle, a face on the screen, and no answer to anything said to it again.
    if (!duplex_.playbackEnded()) return;
    if (beginCapture()) {
        pre_roll_slots_.clear();
        monitoring_ = true;
        // The caller clears its endpointer on the strength of this: the silence during playback
        // belongs to nobody's pause, and the tail of the reply is not the start of a sentence.
        monitor_resumed_ = true;
    }
}

}  // namespace app

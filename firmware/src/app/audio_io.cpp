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

bool AudioIo::begin(uint8_t volume) {
    volume_ = volume;

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
    if (M5.Speaker.isEnabled()) M5.Speaker.end();
    if (!M5.Mic.begin()) return false;

    sink_ = sink;
    tally_.reset();
    capture_slot_ = 0;
    listening_ = true;

    // **Both** slots armed. The microphone has a two-deep queue (`isRecording()` returns 0, 1 or
    // 2), and arming one at a time leaves it idle from the moment a frame completes until `tick`
    // notices and re-arms -- a gap in every frame. Measured, that cost a quarter of the audio: a
    // three-second window produced 2260 ms. With both armed the recorder never stops, and the
    // loop's job is only to drain and re-arm the slot that freed.
    M5.Mic.record(capture_[0], roboface::kCaptureFrameSamples, roboface::kCaptureSampleRate);
    M5.Mic.record(capture_[1], roboface::kCaptureFrameSamples, roboface::kCaptureSampleRate);
    return true;
}

void AudioIo::stopListening() {
    if (!listening_) return;
    listening_ = false;
    sink_ = nullptr;
    if (M5.Mic.isEnabled()) M5.Mic.end();
}

void AudioIo::tick(uint32_t now_ms) {
    if (listening_) {
        // A frame is ready when the recorder has stopped filling it. Sending happens here, in the
        // loop, rather than in an interrupt: the socket write can block, and blocking inside the
        // I2S callback would drop the samples arriving behind it.
        // Fewer than two queued means one has completed, and the slots complete in the order they
        // were armed -- so the one that freed is `capture_slot_`.
        if (M5.Mic.isRecording() < 2) {
            const auto* frame = reinterpret_cast<const uint8_t*>(capture_[capture_slot_]);
            const std::size_t completed = capture_slot_;
            capture_slot_ = capture_slot_ == 0 ? 1 : 0;

            // Re-arm the freed slot **before** sending it onward, so the microphone is back to two
            // queued while this frame is on the wire. Sending first would leave the queue one deep
            // for the duration of the send, which is the same gap in a smaller form.
            M5.Mic.record(capture_[completed], roboface::kCaptureFrameSamples,
                          roboface::kCaptureSampleRate);

            if (sink_ != nullptr && sink_(frame, roboface::kCaptureFrameBytes)) {
                tally_.recordFrame(roboface::kCaptureFrameBytes);
            }
        }
    }

    if (!speaking_) return;

    // Hand over buffers until the speaker refuses one. `playRaw` returning false is the whole
    // pacing mechanism: it means both slots are claimed, and the chunk stays in the ring for the
    // next tick rather than being thrown away.
    while (!backlog_.empty()) {
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

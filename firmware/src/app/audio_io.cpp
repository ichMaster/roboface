#include "app/audio_io.h"

#include <M5Unified.h>

namespace app {
namespace {

//: The device's playback format, and the server's: `AUDIO_FMT` in `protocol.py` is
//: `pcm16/16000/1`, and ElevenLabs is asked for `pcm_16000` so nothing decodes anywhere.
constexpr uint32_t kSampleRate = 16000;

//: How much is handed to the speaker per tick. Small enough that a `tts_end` arriving mid-phrase
//: is acted on promptly, large enough not to make a system call per sample.
constexpr std::size_t kChunkBytes = 1024;

}  // namespace

bool AudioIo::begin(uint8_t volume) {
    volume_ = volume;
    M5.Speaker.setVolume(volume_);
    return true;
}

void AudioIo::startSpeaking() {
    if (speaking_) return;  // the bus is already ours; switching again would glitch the output
    // One bus, two peripherals. M5Unified refuses to run both, so the microphone is released
    // before the speaker is claimed -- and v1.2's capture path will do the reverse.
    if (M5.Mic.isEnabled()) M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume_);
    speaking_ = true;
    draining_ = false;
}

std::size_t AudioIo::write(const uint8_t* data, std::size_t length) {
    return buffer_.write(data, length);
}

void AudioIo::tick() {
    if (!speaking_) return;

    uint8_t chunk[kChunkBytes];
    // Whole samples only: handing the speaker an odd byte count shifts every following sample by
    // a byte, which turns the rest of the phrase to noise rather than producing one click.
    const std::size_t got = buffer_.readSamples(chunk, sizeof(chunk));
    if (got > 0) {
        M5.Speaker.playRaw(reinterpret_cast<const int16_t*>(chunk),
                           got / sizeof(int16_t), kSampleRate, /*stereo=*/false, /*repeat=*/1);
        bytes_played_ += static_cast<uint32_t>(got);
        return;
    }

    // Starved. If the turn is over, this is the end of the audio; otherwise it is a pause and the
    // next frame will fill it. Either way nothing is written -- emitting silence into the middle
    // of a word does not sound like a stall, it sounds like the sentence ended.
    if (draining_ && !M5.Speaker.isPlaying()) releaseBus();
}

void AudioIo::finish() {
    if (!speaking_) return;
    draining_ = true;  // `tick` releases the bus once the buffer and the speaker are both empty
}

void AudioIo::abort() {
    if (!speaking_) {
        buffer_.clear();
        return;
    }
    M5.Speaker.stop();
    buffer_.clear();
    releaseBus();
}

void AudioIo::releaseBus() {
    M5.Speaker.end();
    speaking_ = false;
    draining_ = false;
    buffer_.clear();
}

}  // namespace app

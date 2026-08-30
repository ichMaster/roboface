// RoboFace firmware — entry point.
//
// v0.4 completes the v0 skeleton: a server, a wire, a board, and a face. A **device state is a
// face**, never a word (MISSION, and ROADMAP §v0.4's DoD) — the only text this firmware puts on the
// screen is an enumerated error code, which is an identifier rather than a state.
//
// This file is wiring and stays wiring. Every decision it appears to make is delegated: the state
// machine says what a transition means, `roboface::recipeFor` says what a state looks like,
// `roboface::Chrome` says what is visible, `roboface::LineReader` says where a line ends. What is
// left here is the order things happen in — the one thing a host test cannot check.

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "app/audio_io.h"
#include "app/chrome_view.h"
#include "app/console_view.h"
#include "app/net.h"
#include "app/procedural_renderer.h"
#include "app/sensors.h"
#include "app/ws.h"
#include "config.h"
#include "pure/chrome.h"
#include "pure/console.h"
#include "pure/motion.h"
#include "pure/proximity.h"
#include "pure/ptt.h"
#include "pure/reflex.h"
#include "pure/touch.h"
#include "pure/transcript.h"
#include "pure/vad.h"
#include "pure/line_reader.h"
#include "pure/state.h"
#include "pure/version.h"
#include "pure/ws_protocol.h"
#include "pure/ws_url.h"

namespace {

app::Net net;
app::Ws ws;
app::ProceduralRenderer renderer;
app::ChromeView chrome_view;
app::ConsoleView console_view;
app::AudioIo audio;
app::Imu imu;
app::Proximity proximity;
roboface::Chrome chrome;
roboface::LineReader lines;
roboface::DeviceState state = roboface::DeviceState::kBoot;

//: The server has sent the last of the reply, but the speaker has not yet said it. See the note at
//: the `frame.final` branch: the gap between those two moments is seconds, not milliseconds.
bool reply_end_pending = false;

//: Whether a server `emotion{}` frame is in force. While it is, the device stops choosing its own
//: expression for the turn states -- which is the authority rule this whole phase exists to move.
//: It is dropped when the link goes, because at that point the last thing the server said is no
//: longer a description of anything.
bool server_face = false;

constexpr uint32_t kStatusIntervalMs = 10000;

// Nothing on this screen changes faster than DEVICE_UI's 120 ms chrome fade -- about 8 Hz. 30 Hz is
// generous for a face that is currently static, and it is the difference between the loop serving
// the network and the loop being one long sprite push: 320x240x16 is 153 KB, so the unconditional
// push in every 5 ms iteration implied ~30 MB/s on a bus that carries about 5.
//: The face's frame interval when nothing else needs the loop. 100 ms is 10 FPS.
//:
//: **Below the 15-20 the roadmap asks for, deliberately, and measured.** At 18 FPS the face held
//: its rate and capture collapsed to 35% of a window -- 260 frames where 750 were due. A 320x240
//: 16-bit sprite is 150 KB, so each push is 150 KB over SPI; the rate is not the cost, the pushes
//: are.
//:
//: And 10 FPS is enough for what this animation actually does. The breath moves 3 px over 4.2 s
//: and the gaze 4 px over 9 s -- at 10 FPS that is 42 and 90 frames respectively, far more than
//: either needs. The one motion that wants speed is the blink, and it is handled by lengthening
//: the blink rather than by raising the rate for everything else.
constexpr uint32_t kMinPushIntervalMs = 100;

//: The face's frame interval **while the audio path is busy** -- capturing or speaking.
//:
//: Measured, not chosen. At 55 ms the face held 16 FPS and capture collapsed to 35% of a window.
//: 300 ms is three frames a second: during a turn the face is holding one expression anyway.
//:
//: The rule RF-058 was written for: **the face may drop frames, the microphone may not.** During a
//: turn the face is holding one expression anyway -- the idle loop is the thing that wants frames,
//: and the idle loop is exactly what is not happening while someone is talking.
//:
//: There is a second, less obvious reason this matters. The endpointer measures time in *frames*,
//: so dropped frames slow its clock: at a third of the capture rate its 1200 ms end-pause becomes
//: nearly four real seconds, and the window stops closing on its own.
//: While speaking. The mouth follows the reply's loudness, so this is the rate the lip-sync gets.
//:
//: **40 ms, measured at 20 FPS on the board** -- the top of the 15-20 roadmap §v2.3 asks for. Two
//: intermediate values were tried and the numbers are in `v2.3-execution-report.md`: 120 ms gave 8
//: FPS and 7-16 mouth changes per ten seconds, 60 ms gave 11 and 23-35, and 40 ms gives 20 and 88.
//: The interval was the binding constraint throughout, which was worth establishing rather than
//: assuming -- the push cost could as easily have been the ceiling.
//:
//: Affordable for a reason that is specific rather than optimistic: **half-duplex suspends the
//: microphone during playback**, so these frames compete with nothing. Measured rather than
//: predicted: `drop=0 ref=0` through a thirty-second reply, and `mic` in the surrounding listening
//: windows is 45-47, exactly where it was before.
//:
//: The rule underneath has not moved: **the face may drop frames, the microphone may not.** What
//: changed is that this is the one state where the microphone is not running.
constexpr uint32_t kBusyPushIntervalMs = 40;

//: While listening. Faster than `kBusyPushIntervalMs` for one reason and one only: the level meter
//: is in this band, and it is the only thing on the screen that moves with the person's own voice.
//: At three frames a second it lags visibly behind what they are saying, which makes it a worse
//: signal of attention than no meter at all.
//:
//: The face itself is frozen in this state, so these frames buy the meter and nothing else.
constexpr uint32_t kListeningPushIntervalMs = 150;
uint32_t last_push_ms = 0;
bool needs_push = true;

// The AXP2101 answers over I2C, and a battery percentage moves on the scale of minutes. Polling it
// at the loop rate was ~400 transactions a second, forever, on a bus that v2's IMU and v2.4's touch
// controller also want.
constexpr uint32_t kPowerPollIntervalMs = 2000;
uint32_t last_power_poll_ms = 0;
int battery_percent = 100;
bool battery_charging = false;
uint32_t last_status_ms = 0;
bool reply_in_progress = false;
bool debug_line = false;  // the tiny corner diagnostic; off by default

//: When a `/listen` window should close on its own. Zero means no timed window is open. The touch
//: trigger in RF-037 ends on release instead; this exists so capture can be driven from a script.
uint32_t listen_until_ms = 0;

//: Interims arrive several times a second and serial is slow. Four a second is enough to watch
//: recognition working and cheap enough not to starve capture.
constexpr uint32_t kPartialLogIntervalMs = 250;
uint32_t last_partial_log_ms = 0;

//: How often the level meter asks for a repaint while listening. Ten a second looks smooth and
//: leaves the loop time to service the microphone.
constexpr uint32_t kMeterIntervalMs = 100;
uint32_t last_meter_ms = 0;
uint32_t now_ms_for_log = 0;

//: Press-and-hold on the glass. The rules live in `pure/ptt.h`; this holds the instance and the
//: panel is read once per loop.
roboface::PushToTalk ptt;

//: The affection half of the same finger (v2.4). See the note at the call site for why there are
//: two classifiers over one panel.
roboface::TouchGestures gestures;

//: How many fingers were on the glass last loop. Mute is the two-finger tap from v2.4, and a count
//: is the only way to tell it from the single tap that is now affection.
uint8_t fingers_last = 0;

//: When a `/loopback` recording should stop and play back. Zero means none is running.
uint32_t loopback_until_ms = 0;

// The serial chat console (v0.5). It borrows the screen the way the self-test does, and gives back
// the state it took -- the discipline lives in `pure/console.h` where a host test proves it is
// total over the state enum.
//: Step 2: the endpointer hears every frame and its decisions are counted, not acted on.
//: Whether the room is being listened to at all. Toggled by a tap; off is v1.3's
//: behaviour -- press and hold to talk.
bool active_listening = ACTIVE_LISTENING != 0;

roboface::Endpointer endpointer;
uint32_t vad_starts = 0;
uint32_t vad_ends = 0;
//: What the endpointer decided, acted on by the loop rather than inside the audio path -- opening
//: a window from there would re-enter `audio.tick` while it is draining a frame.
roboface::VadEvent pending_vad = roboface::VadEvent::kNone;
//: The loudest frame and its zero-crossing count since the last status line. The endpointer's two
//: thresholds are exactly these two numbers, so printing them turns picking a threshold from
//: guesswork into reading the room.
float peak_recent = 0.0f;
std::size_t crossings_recent = 0;

//: Calibration: per-frame peak (as a percent) and zero-crossing count, collected over a fixed
//: window so the two thresholds the endpointer uses can be *read off the room* instead of guessed.
//: One byte and two per frame -- 750 frames is 15 s and costs 2.2 KB, which internal RAM can spare.
constexpr std::size_t kCalFrames = 750;
uint8_t cal_peak[kCalFrames] = {};
uint16_t cal_zc[kCalFrames] = {};
std::size_t cal_count = 0;
uint32_t cal_until_ms = 0;
const char* cal_label = "";

//: The `pct`-th percentile of a sorted copy. Percentiles rather than a mean: what matters for a
//: threshold is where the bulk of the frames sit, and one door slam should not move it.
uint8_t percentileOf(uint8_t* values, std::size_t count, int pct) {
    if (count == 0) return 0;
    for (std::size_t i = 1; i < count; ++i) {  // insertion sort: count is small and this is a test
        const uint8_t key = values[i];
        std::size_t j = i;
        while (j > 0 && values[j - 1] > key) { values[j] = values[j - 1]; --j; }
        values[j] = key;
    }
    std::size_t index = static_cast<std::size_t>(count * pct / 100);
    if (index >= count) index = count - 1;
    return values[index];
}
//: When the open window opened, so one that never ends can be ended. The server's 30 s size cap
//: would otherwise end it as a protocol error and leave the device sitting in `error`.
uint32_t listen_opened_ms = 0;

//: Counters sampled at each status line, so the rates printed are per-interval rather than
//: since-boot averages -- an average since boot hides a regression that started a minute ago.
uint32_t frames_at_status = 0;
uint32_t capture_at_status = 0;

//: Frames the recorder has produced since boot, counted in the observer. Monotonic on purpose --
//: unlike the send tally, which resets at every window and underflowed into four billion the first
//: time it was subtracted against a stale baseline.
uint32_t frames_captured = 0;
uint32_t captured_at_status = 0;
constexpr uint32_t kMaxWindowMs = 15000;

roboface::ConsoleMode console;
roboface::Transcript transcript;

// The prompt is printed after entering the mode and after each reply settles, so the serial
// session reads as a conversation rather than as a log you occasionally type into.
void printConsolePrompt() {
    if (console.isOn()) Serial.print("\nchat> ");
}

// The self-test. Without a reachable server, idle/thinking/replying cannot be entered by any
// natural route — and DEVICE_UI's whole point is that the screen never says which state it is in,
// so a person checking the faces needs the *serial* to say what the screen should be showing.
bool self_test_running = false;
int self_test_index = 0;
uint32_t self_test_next_ms = 0;
// What the device was actually doing before the self-test borrowed the screen. Without this the
// cycle ends leaving the device in `error` — it says "back to the real state" and is not, which is
// worse than not saying it, because the status line then disagrees with the screen forever.
roboface::DeviceState self_test_saved_state = roboface::DeviceState::kIdle;
constexpr uint32_t kSelfTestHoldMs = 2500;
constexpr roboface::DeviceState kSelfTestStates[] = {
    roboface::DeviceState::kIdle,     roboface::DeviceState::kListening,
    roboface::DeviceState::kThinking, roboface::DeviceState::kReplying,
    roboface::DeviceState::kOffline,  roboface::DeviceState::kError,
};

//: Which states the device still decides for itself.
//:
//: **The line is "can the server know this".** `boot` and `wifi_connecting` happen before there is
//: a server; `offline` is by definition the server being unreachable; a local fault may be a fault
//: in reaching it. For those four the device must have a face of its own or it would have none.
//:
//: `idle`, `listening`, `thinking` and `replying` are the turn, and from v2.2 the turn's face is
//: the server's to choose. That is the authority move this whole phase is.
bool isDeviceOwnedState(roboface::DeviceState state) {
    return state == roboface::DeviceState::kBoot ||
           state == roboface::DeviceState::kWifiConnecting ||
           state == roboface::DeviceState::kOffline ||
           state == roboface::DeviceState::kError;
}

void render() {
    // The console borrows the face area; chrome keeps its bands either way, because link and
    // battery are facts and hiding them would hide a dropped link exactly when it matters.
    if (console.isOn()) {
        console_view.draw(renderer.canvas(), transcript);
    } else if (!server_face || isDeviceOwnedState(state)) {
        // The device's own face, for the states that are the device's own. Everything inside a
        // turn is targeted when the server's frame arrives, not here -- calling `show` every
        // render would restart the frame's ttl on every repaint and it would never expire.
        renderer.show(state);
    }
    chrome_view.draw(renderer.canvas(), chrome, audio.inputLevel());
    if (debug_line && renderer.canvas() != nullptr) {
        renderer.canvas()->setTextColor(0x39E7, 0x0000);
        // Explicit, for the same reason chrome_view.cpp is: this line inherited the console's font
        // and was drawn at twice its intended size. (v0.5 review, finding 2.)
        renderer.canvas()->setFont(&fonts::Font0);
        renderer.canvas()->setTextSize(1);
        renderer.canvas()->setTextDatum(top_left);
        renderer.canvas()->drawString(roboface::toString(state), 4, 4);
    }
    renderer.push();
}

void apply(roboface::DeviceEvent event) {
    // Anything else that lands -- a tap to listen, a dropped socket, a fault -- overtakes the wait:
    // the reply the device was finishing is no longer what it is doing.
    if (event != roboface::DeviceEvent::kTurnEnded) reply_end_pending = false;

    const roboface::Transition result = roboface::transition(state, event);
    if (!result.accepted) return;

    state = result.next;
    Serial.printf("\n[state] %s\n", roboface::toString(state));
    render();
    needs_push = true;
}

// One captured frame, straight onto the wire. Returning false tells `AudioIo` the frame did not
// go; capture keeps running rather than stalling, because the microphone does not pause for the
// network and a gap is better than losing the rest of the sentence.
//: Кадри, які сокет не взяв. Лічильник тут, а не в `AudioIo`: відмова належить лінку, і без неї
//: «надіслано 1 кадр» неможливо відрізнити від «мікрофон замовк після одного кадру» -- дві зовсім
//: різні поломки з однаковим числом.
uint32_t frames_refused = 0;

bool sendCapturedFrame(const uint8_t* data, std::size_t length) {
    const bool sent = ws.sendAudio(data, length);
    if (!sent) ++frames_refused;
    return sent;
}

// The loopback sink: the same frames, into the playback buffer instead of onto the wire.
bool storeCapturedFrame(const uint8_t* data, std::size_t length) {
    return audio.captureToBacklog(data, length) == length;
}

void onCapturedFrame(const int16_t* samples, std::size_t count, uint32_t frame_ms) {
    // Every frame the recorder produces, whether or not a window is open.
    //
    // `cap` in the status line counts frames *sent*, so it reads zero whenever nothing is
    // listening -- which hides the question that matters most: is the microphone keeping up while
    // the face animates? A VAD that never triggers looks identical whether the room is quiet or
    // the recorder is starved, and those need opposite fixes.
    ++frames_captured;

    const float peak = roboface::peakLevel(samples, count);
    if (cal_until_ms != 0 && cal_count < kCalFrames) {
        cal_peak[cal_count] = static_cast<uint8_t>(peak * 100.0f);
        cal_zc[cal_count] = static_cast<uint16_t>(roboface::zeroCrossings(samples, count));
        ++cal_count;
    }
    // Only while calibrating. This runs on **every** captured frame, and the endpointer already
    // scans the same 320 samples for the same thing -- doing it twice more for a status line cost
    // a quarter of the frames in a window, which reaches the recogniser as speech full of holes.
    // A diagnostic must not compete with the thing it is diagnosing.
    if (peak > peak_recent) {
        peak_recent = peak;
        if (cal_until_ms != 0) crossings_recent = roboface::zeroCrossings(samples, count);
    }
    switch (endpointer.feed(samples, count, frame_ms)) {
        case roboface::VadEvent::kSpeechStarted:
            ++vad_starts;
            pending_vad = roboface::VadEvent::kSpeechStarted;
            break;
        case roboface::VadEvent::kSpeechEnded:
            ++vad_ends;
            pending_vad = roboface::VadEvent::kSpeechEnded;
            break;
        default: break;
    }
}

// Open the listening window: the frame first, then the microphone. In that order, because a device
// that captured before announcing would have audio with no window to put it in, and the server
// would rightly call it a protocol violation.
bool beginListening() {
    if (audio.isListening()) return true;
    if (!ws.isConnected()) {
        Serial.println("[listen] no connection — not listening");
        return false;
    }
    if (!roboface::canStartTurn(state)) {
        Serial.printf("[busy] not idle (%s) — not listening\n", roboface::toString(state));
        return false;
    }
    if (!ws.sendListenStart()) {
        Serial.println("[listen] could not open the window");
        return false;
    }
    if (!audio.startListening(&sendCapturedFrame)) {
        // The window is open on the server and the microphone did not start. Close it rather than
        // leaving the server waiting for audio that will never arrive.
        ws.sendListenStop();
        Serial.println("[listen] microphone did not start");
        return false;
    }
    const std::size_t pre_rolled = audio.flushPreRoll();
    if (pre_rolled > 0) {
        Serial.printf("[listen] pre-roll %u кадрів\n", static_cast<unsigned>(pre_rolled));
    }
    listen_opened_ms = millis();
    apply(roboface::DeviceEvent::kListenStarted);
    return true;
}

// Close it: the microphone first, then the frame, so the last captured samples are on the wire
// before the server is told the utterance is over.
void endListening(const char* why = "?") {
    if (!audio.isListening()) return;
    audio.stopListening();
    ws.sendListenStop();
    Serial.printf("[listen] closed by %s після %lu мс · sent %u frames (%u ms) · vad s=%u e=%u\n",
                  why, static_cast<unsigned long>(listen_opened_ms == 0 ? 0u : millis() - listen_opened_ms),
                  static_cast<unsigned>(audio.tally().frames()),
                  static_cast<unsigned>(audio.tally().durationMs()),
                  static_cast<unsigned>(vad_starts), static_cast<unsigned>(vad_ends));
    Serial.printf("[listen] відмов сокета: %lu\n", static_cast<unsigned long>(frames_refused));
    frames_refused = 0;
    // Leaves the device *thinking*, which is now correct as written: recognition follows, then a
    // reply, and the terminal `reply` frame ends the turn. v1.2 added a kTurnEnded here because it
    // had neither; v1.3 removes it, as that comment said it would.
    listen_opened_ms = 0;
    endpointer.reset();
    pending_vad = roboface::VadEvent::kNone;
    apply(roboface::DeviceEvent::kListenStopped);
}

//: Turn active listening on or off, and say so. One implementation for the tap and for `/mic`:
//: two copies of this would drift the first time one of them learned to close an open window.
void setActiveListening(bool wanted) {
    if (wanted == active_listening) {
        Serial.printf("\n[mic] активне слухання вже %s\n", wanted ? "увімкнено" : "вимкнено");
        return;
    }
    active_listening = wanted;
    if (active_listening) {
        if (!audio.startMonitoring(&onCapturedFrame)) {
            Serial.println("[vad] мікрофон не запустився — лишаюсь вимкненим");
            active_listening = false;
        } else {
            endpointer.reset();
            pending_vad = roboface::VadEvent::kNone;
        }
    } else {
        // A window already open is closed with it: switching the microphone off must mean off,
        // not "off after this sentence".
        if (audio.isListening()) endListening("mic");
        audio.stopMonitoring();
    }
    Serial.printf("\n[mic] активне слухання %s\n",
                  active_listening ? "увімкнено" : "вимкнено");
    needs_push = true;  // the indicator changes now, not at the next repaint
}

//: The tap's form: no argument to give, so it flips. `/mic on` and `/mic off` exist for scripts,
//: which cannot see the indicator and so cannot use a toggle.
void toggleActiveListening() { setActiveListening(!active_listening); }

//: Which reflex a gesture fires. The mapping is DEVICE_UI §Input's, one line each.
roboface::Reflex reflexFor(roboface::TouchGesture gesture) {
    switch (gesture) {
        case roboface::TouchGesture::kTap:
        case roboface::TouchGesture::kMultiTap: return roboface::Reflex::kTickle;
        case roboface::TouchGesture::kStroke: return roboface::Reflex::kContented;
        case roboface::TouchGesture::kPokeEye: return roboface::Reflex::kStartle;
        default: return roboface::Reflex::kNone;
    }
}

//: And which `kind` it is on the wire. `long_press` and `held_silent` are **control** gestures --
//: DEVICE_UI is explicit that control is local UI and is deliberately not reported, because the
//: character has no business knowing the person opened a menu.
const char* eventKindFor(roboface::TouchGesture gesture) {
    switch (gesture) {
        case roboface::TouchGesture::kTap: return "tap";
        case roboface::TouchGesture::kMultiTap: return "multi_tap";
        case roboface::TouchGesture::kStroke: return "stroke";
        case roboface::TouchGesture::kPokeEye: return "poke_eye";
        default: return nullptr;
    }
}

const char* zoneName(roboface::TouchZone zone) {
    switch (zone) {
        case roboface::TouchZone::kForehead: return "forehead";
        case roboface::TouchZone::kEye: return "eye";
        case roboface::TouchZone::kCheek: return "cheek";
        default: return "outside";
    }
}

//: **Both levels, in the order their deadlines demand.** The reflex fires first and unconditionally
//: -- it is the whole point of the first level that it does not wait for a network -- and the
//: report goes second and may fail without anyone noticing.
void onTouchGesture(const roboface::TouchResult& touched, uint32_t now_ms) {
    const roboface::Reflex reflex = reflexFor(touched.gesture);
    if (reflex != roboface::Reflex::kNone) {
        if (touched.gesture == roboface::TouchGesture::kTap ||
            touched.gesture == roboface::TouchGesture::kMultiTap) {
            renderer.recordTap(now_ms);
        }
        renderer.fireReflex(reflex, now_ms);
        needs_push = true;
    }

    const char* kind = eventKindFor(touched.gesture);
    if (kind == nullptr) return;  // a control gesture: local UI, deliberately not reported

    Serial.printf("\n[touch] %s %s x%u\n", kind, zoneName(touched.zone),
                  static_cast<unsigned>(touched.count));
    ws.sendEvent(roboface::EventType::kTouch, kind, "zone", zoneName(touched.zone),
                 "count", touched.count);
}

//: A motion, and the reflex it deserves. Free fall and being turned over are disorienting; a shake
//: is a startle.
//: A hand arrived or left. **This one is a reflex first and an event a distant second**: the face
//: turning toward a hand is the reaction, and the remark the server might make about it is a bonus
//: that arrives a second later. With the server gone the turn still happens, which is the DoD's
//: last clause and the whole reason the levels are separate mechanisms.
void onPresence(roboface::Presence presence, uint32_t now_ms) {
    const bool near = presence == roboface::Presence::kApproach;
    renderer.setGazeReflex(near, near ? roboface::kGazePull : 0.0f);
    if (near) renderer.fireReflex(roboface::Reflex::kAlert, now_ms);
    needs_push = true;

    Serial.printf("\n[near] %s\n", roboface::toString(presence));
    ws.sendEvent(roboface::EventType::kProximity, roboface::toString(presence));
}

void onMotion(roboface::Motion motion, uint32_t now_ms) {
    switch (motion) {
        case roboface::Motion::kShake:
            renderer.fireReflex(roboface::Reflex::kStartle, now_ms);
            break;
        case roboface::Motion::kFreeFall:
        case roboface::Motion::kUpsideDown:
            renderer.fireReflex(roboface::Reflex::kDizzy, now_ms);
            break;
        default:
            break;
    }
    needs_push = true;
    Serial.printf("\n[motion] %s\n", roboface::toString(motion));
    ws.sendEvent(roboface::EventType::kMotion, roboface::toString(motion));
}


roboface::LinkState linkStateNow() {
    if (ws.isConnected()) return roboface::LinkState::kConnected;
    if (net.isUp()) return roboface::LinkState::kDegraded;  // link but no socket
    return roboface::LinkState::kOffline;
}

void pollPower(uint32_t now_ms, bool force) {
    if (!force && now_ms - last_power_poll_ms < kPowerPollIntervalMs) return;
    last_power_poll_ms = now_ms;
    battery_percent = M5.Power.getBatteryLevel();
    battery_charging = M5.Power.isCharging();
}

void updateChrome(uint32_t now_ms, bool fault_active, roboface::ErrorCode fault) {
    const roboface::ChromeVisibility before = chrome.visibility();

    roboface::ChromeFacts facts;
    facts.link = linkStateNow();
    facts.battery_percent = battery_percent;
    facts.charging = battery_charging;
    facts.fault_active = fault_active;
    facts.fault = fault;
    // The meter is wanted exactly while the microphone is open. The band arbitrates -- a fault
    // outranks it, per DEVICE_UI -- so this states a want rather than a decision.
    facts.level_meter_wanted = audio.isListening();
    facts.mic_muted = !active_listening;
    chrome.update(now_ms, facts);

    const roboface::ChromeVisibility after = chrome.visibility();
    // Repaint when what is shown changes, and while a fade is running -- the fade is the only thing
    // that needs frames without a change behind it.
    if (before.link != after.link || before.battery != after.battery || before.band != after.band) {
        needs_push = true;
    }
    const uint32_t settled = chrome.settledForMs();
    if (settled < roboface::kSettleHideMs + roboface::kChromeFadeOutMs) needs_push = true;
}

bool fault_active = false;
roboface::ErrorCode fault_code = roboface::ErrorCode::kUnknown;

void onSocketEvent(app::Ws::Event event, const roboface::ServerFrame& frame) {
    switch (event) {
        case app::Ws::Event::kConnected:
            fault_active = false;
            apply(roboface::DeviceEvent::kSocketUp);
            return;
        case app::Ws::Event::kDisconnected:
            if (reply_in_progress) {
                Serial.println();
                reply_in_progress = false;
            }
            ptt.cancel();
            if (audio.isListening()) audio.stopListening();
            // The device takes its face back. Whatever the server last said described a turn that
            // is now unreachable, and holding it would leave the device cheerful about a
            // connection that had dropped.
            server_face = false;
            apply(roboface::DeviceEvent::kSocketLost);
            return;
        case app::Ws::Event::kDropped:
            return;
        case app::Ws::Event::kFrame:
            break;
    }

    switch (frame.result) {
        case roboface::ParseResult::kReply:
            // v0.3 code review, finding 5: a reply outside a turn used to print as though a turn
            // were running, so the text and the device state disagreed. From this version state is
            // on a screen a person is looking at, which is what makes the disagreement matter.
            if (state != roboface::DeviceState::kThinking &&
                state != roboface::DeviceState::kReplying) {
                Serial.printf("\n[warn] reply outside a turn (%s) — ignored\n",
                              roboface::toString(state));
                return;
            }
            if (frame.final) {
                if (reply_in_progress) Serial.println();
                reply_in_progress = false;
                // **The turn ends when the device has finished speaking, not when the server has
                // finished sending.** Those are seconds apart: the server streams TTS faster than
                // real time and the device buffers what it cannot play yet -- measured at 244 KB,
                // which is about eight seconds of voice still to come after this frame arrives.
                //
                // Ending the turn here put the face into `idle` while the speaker was mid-sentence:
                // the drift and the blink came back, and the mouth -- which only follows the audio
                // while the state is `replying` -- froze open for the rest of the reply. Both were
                // reported as separate faults; they are this one line.
                //
                // The wait is bounded by the drain timeout in `AudioIo`, which releases the bus and
                // clears `isSpeaking()` even if the speaker never reports going quiet.
                if (audio.isSpeaking()) {
                    reply_end_pending = true;
                    printConsolePrompt();
                    return;
                }
                apply(roboface::DeviceEvent::kTurnEnded);
                // The turn is over; invite the next one. Only when the console is on -- outside it
                // the serial session is a log, and a prompt would be noise in it.
                printConsolePrompt();
                return;
            }
            if (!reply_in_progress) {
                apply(roboface::DeviceEvent::kReplyStarted);
                Serial.print("  ");
                reply_in_progress = true;
            }
            // Written inside the receive path, not accumulated: the first words appear while the
            // last are still being generated, which is the property v0.2 exists to provide.
                Serial.print(frame.text.c_str());
                // The console accumulates and marks the sprite dirty; the loop's 33 ms
                // rate limit decides when to repaint. Pushing per delta would put the
                // loop back to being one long sprite push, which v0.4's review removed.
                transcript.appendReply(frame.text.c_str());
                needs_push = true;
                return;

        case roboface::ParseResult::kAsrPartial:
            // Serial only. DEVICE_UI and MISSION are unchanged by a transcript: the screen shows
            // states, and a guess about what you said is the last thing that belongs on it.
            //
            // **Rate-limited, because serial is slow and capture is not optional.** At 115200 baud
            // a growing transcript printed on every interim blocks the loop that polls the
            // microphone, and the frames it misses are simply gone: a four-second utterance
            // arrived as 1.9 s of audio, evenly sampled, which recognition returns nothing for.
            // A debug line must never cost the thing it is debugging.
            if (now_ms_for_log - last_partial_log_ms >= kPartialLogIntervalMs) {
                last_partial_log_ms = now_ms_for_log;
                Serial.printf("\r[heard?] %s", frame.text.c_str());
            }
            return;

        case roboface::ParseResult::kAsr:
            Serial.printf("\n[heard] %s\n", frame.text.c_str());
            // **The utterance is over.** The recogniser decided it, and it decides better than the
            // local end-pause: it endpoints on ~500 ms of silence, while this device's pause is
            // reset by any scrap of room noise loud enough to pass the threshold.
            //
            // Without this the device stays in `listening`, and everything that follows fails in a
            // way that looks unrelated: the reply is refused as arriving "outside a turn", and the
            // speaker cannot take the shared bus because the microphone still holds it
            // (`I2S: register I2S object to platform failed`). The person simply hears nothing.
            if (audio.isListening()) endListening("asr");
            return;

        case roboface::ParseResult::kEmotion:
            // **The whole of v2.2 on the device, in three lines.** No rule here decides what the
            // face means; the server decided, and this hands the decision to the renderer.
            //
            // The device's own states keep their own faces -- `boot`, `wifi_connecting`, `offline`
            // and a local fault are facts about the link, and one of them is by definition a fact
            // about the server being unreachable. Everything inside a turn is the server's.
            server_face = true;
            renderer.show(frame.emotion);
            needs_push = true;
            Serial.printf("\n[face] %s %d%%%s\n", roboface::toString(frame.emotion.emotion),
                          static_cast<int>(frame.emotion.intensity * 100.0f),
                          frame.emotion.speaking ? " speaking" : "");
            return;

        case roboface::ParseResult::kTtsEnd:
            // The speaking window is closed. Drain what is buffered, then give the shared I2S bus
            // back -- v1.2's capture path needs it, and a turn that kept it would break a
            // subsystem that is working correctly.
            audio.finish();
            return;

        case roboface::ParseResult::kError:
            if (reply_in_progress) {
                Serial.println();
                reply_in_progress = false;
            }
            // The server owns the window; a hold is only a request for one. Any refusal while
            // listening ends the capture -- otherwise the device keeps sending into a window that
            // is already closed and earns another `bad_frame` for every frame, a round trip each,
            // for as long as the finger stays down. (v1.2 review, finding 2.)
            if (audio.isListening()) {
                Serial.println("[listen] server refused the window — capture stopped");
                audio.stopListening();
                ptt.cancel();
                listen_until_ms = 0;
            }
            // Speech that has been superseded is worse than silence: it answers a question
            // nobody is still asking.
            audio.abort();
            // The window is already gone with the turn; abandon the hold rather than reporting a
            // stop for something the server has stopped listening to.
            ptt.cancel();
            if (audio.isListening()) audio.stopListening();
            fault_active = true;
            fault_code = frame.code;
            Serial.printf("\n[error] %s: %s\n", roboface::toString(frame.code), frame.msg.c_str());
            apply(roboface::DeviceEvent::kFault);
            return;

        default:
            return;
    }
}

void startSelfTest() {
    self_test_running = true;
    self_test_index = 0;
    self_test_next_ms = 0;
    self_test_saved_state = state;
    Serial.println("\n[faces] cycling the six DoD states; the screen shows only the face.");
}

void stepSelfTest(uint32_t now_ms) {
    if (!self_test_running || now_ms < self_test_next_ms) return;

    constexpr int count = sizeof(kSelfTestStates) / sizeof(kSelfTestStates[0]);
    if (self_test_index >= count) {
        self_test_running = false;
        state = self_test_saved_state;
        Serial.printf("[faces] done — back to %s.\n", roboface::toString(state));
        render();
        return;
    }

    state = kSelfTestStates[self_test_index];
    needs_push = true;
    // Announced on serial because the screen deliberately never says which state it is in; this is
    // how a person checks that what they are looking at is what it should be.
    Serial.printf("[faces] %d/%d  %s\n", self_test_index + 1, count, roboface::toString(state));
    render();

    ++self_test_index;
    self_test_next_ms = now_ms + kSelfTestHoldMs;
}

void handleLine(const roboface::LineReader::Line& line, uint32_t now_ms) {
    if (line.truncated) {
        Serial.printf("[warn] line truncated to %u characters\n",
                      static_cast<unsigned>(line.text.size()));
    }

    if (line.text == "/http") {
        // The same question as /probe, asked with a library nobody can accuse of being
        // hand-rolled: Arduino's HTTPClient, a plain GET, no WebSocket anywhere. If this fails
        // too then neither the protocol nor this repository's code is the variable.
        const roboface::WsUrl url = roboface::parseWsUrl(SERVER_URL);
        char target[96];
        snprintf(target, sizeof(target), "http://%s:%u/", url.host.c_str(),
                 static_cast<unsigned>(url.port));
        Serial.printf("[http] GET %s (Arduino HTTPClient)\n", target);

        HTTPClient http;
        http.setConnectTimeout(5000);
        http.setTimeout(5000);
        const uint32_t started = millis();
        if (!http.begin(target)) {
            Serial.println("[http] begin() refused the URL");
            return;
        }
        const int code = http.GET();
        const uint32_t took = millis() - started;
        if (code > 0) {
            const String body = http.getString();
            Serial.printf("[http] HTTP %d in %lu ms, %u bytes back — the path WORKS\n", code,
                          static_cast<unsigned long>(took),
                          static_cast<unsigned>(body.length()));
        } else {
            Serial.printf("[http] failed in %lu ms: %d (%s)\n", static_cast<unsigned long>(took),
                          code, HTTPClient::errorToString(code).c_str());
        }
        http.end();
        return;
    }

    if (line.text == "/probe") {
        // A raw TCP connect to the configured server, reporting what actually happened.
        // "connection refused" and "timed out" point at completely different problems -- one
        // means nothing is listening, the other that the packets are not arriving at all -- and
        // the WebSocket client's own retry log cannot tell them apart.
        const roboface::WsUrl url = roboface::parseWsUrl(SERVER_URL);
        if (!url.valid) {
            Serial.printf("[probe] SERVER_URL is not usable: %s\n", SERVER_URL);
            return;
        }
        Serial.printf("[probe] TCP connect to %s:%u ...\n", url.host.c_str(),
                      static_cast<unsigned>(url.port));
        WiFiClient probe;
        const uint32_t started = millis();
        const int ok = probe.connect(url.host.c_str(), url.port, 5000);
        const uint32_t took = millis() - started;
        if (ok == 1) {
            // A completed TCP handshake proves almost nothing. A filtering middlebox will accept
            // the connection and reset the payload, so "connected" was reported for a path that
            // could not carry a single byte -- which is exactly how this probe misled its author.
            // Send a real request and require a real answer.
            probe.printf("GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
                         url.path.c_str(), url.host.c_str(), static_cast<unsigned>(url.port));
            const uint32_t wait_until = millis() + 4000;
            String reply;
            while (millis() < wait_until && reply.length() < 40) {
                while (probe.available()) reply += static_cast<char>(probe.read());
                delay(10);
            }
            probe.stop();
            const uint32_t total = millis() - started;
            if (reply.length() == 0) {
                Serial.printf("[probe] TCP connected in %lu ms but the server sent NOTHING back "
                              "(%lu ms total)\n",
                              static_cast<unsigned long>(took),
                              static_cast<unsigned long>(total));
                Serial.println("[probe] a middlebox is accepting the connection and dropping the "
                               "payload — the host is reachable, the service is not");
            } else {
                reply.replace("\r", "");
                const int line_end = reply.indexOf('\n');
                Serial.printf("[probe] OK in %lu ms — server said: %s\n",
                              static_cast<unsigned long>(total),
                              reply.substring(0, line_end > 0 ? line_end : 40).c_str());
            }
        } else {
            Serial.printf("[probe] FAILED after %lu ms (errno %d: %s)\n",
                          static_cast<unsigned long>(took), errno, strerror(errno));
            Serial.println("[probe] fast failure = refused (nothing listening / blocked by the "
                           "host); ~5000 ms = no reply at all (blocked in the network)");
        }
        Serial.printf("[probe] this board is %s, gateway %s\n",
                      WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
        return;
    }

    if (line.text == "/chat-on") {
        // One borrower of the screen at a time. With both running, the self-test would keep
        // assigning states nobody can see -- the console owns the panel -- so its whole purpose
        // silently does nothing, and its own restore fights the console's rendering.
        // (v0.5 review, finding 3.)
        if (self_test_running) {
            Serial.println("[busy] self-test running — /chat-on ignored");
            return;
        }
        if (!console.enable()) {
            Serial.println("[chat] already on");
            printConsolePrompt();
            return;
        }
        transcript.clear();
        Serial.println("\n[chat] console on — type a message; /chat-off returns the face.");
        render();
        needs_push = true;
        printConsolePrompt();
        return;
    }
    if (line.text == "/chat-off") {
        if (!console.disable()) {
            Serial.println("[chat] already off");
            return;
        }
        // Render the state the device is *actually* in, not the one it was in when the console
        // opened. The state machine kept running while the transcript had the screen -- a link can
        // have dropped -- and putting the old state back would make the face assert something
        // untrue. (v0.5 review, finding 1.)
        Serial.printf("\n[chat] console off — showing %s.\n", roboface::toString(state));
        render();
        needs_push = true;
        return;
    }

    if (line.text.rfind("/loopback", 0) == 0) {
        // A diagnostic, never a feature: it sends nothing to the server, and MISSION's non-goals
        // do not gain a voice recorder. It exercises the shared I2S bus in *both* directions in
        // one command, which is the part most likely to be wrong and the slowest to notice
        // through the network path.
        unsigned seconds = 3;
        const std::size_t space = line.text.find(' ');
        if (space != std::string::npos) {
            const int parsed = std::atoi(line.text.c_str() + space + 1);
            if (parsed > 0 && parsed <= 10) seconds = static_cast<unsigned>(parsed);
        }
        // Clamp to what the backlog can actually hold, and say so. The buffer is the internal
        // fallback (PSRAM reports zero free on this board -- v1.1 review, finding 3), which is
        // about 1.5 s of 16 kHz PCM16. Recording longer than that would fill it and drop the rest
        // silently, and a diagnostic that lies about what it captured is worse than no diagnostic.
        const unsigned capacity_s =
            static_cast<unsigned>(audio.backlogCapacity() / (roboface::kCaptureSampleRate * 2));
        if (capacity_s > 0 && seconds > capacity_s) {
            Serial.printf("[loopback] buffer holds %u s — recording that instead of %u\n",
                          capacity_s, seconds);
            seconds = capacity_s;
        }
        Serial.printf("\n[loopback] recording %u s — speak now\n", seconds);
        loopback_until_ms = millis() + seconds * 1000;
        audio.startListening(&storeCapturedFrame);
        return;
    }

    if (line.text.rfind("/listen", 0) == 0) {
        // A trigger before the touch panel has one (RF-037). Also the only way to exercise capture
        // without a finger on the glass, which is what makes a failure reproducible from a script.
        unsigned seconds = 3;
        const std::size_t space = line.text.find(' ');
        if (space != std::string::npos) {
            const int parsed = std::atoi(line.text.c_str() + space + 1);
            if (parsed > 0 && parsed <= 20) seconds = static_cast<unsigned>(parsed);
        }
        Serial.printf("\n[listen] holding for %u s\n", seconds);
        beginListening();
        listen_until_ms = millis() + seconds * 1000;
        return;
    }

    if (line.text == "/mem") {
        // The I2S DMA buffers are allocated from *internal* RAM, so what is left of it decides
        // whether the microphone starts at all -- and `Mic.begin()` reports success either way.
        Serial.printf("[mem] internal free=%u largest=%u min_ever=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        Serial.printf("[mem] psram found=%d size=%u free=%u largest=%u\n", (int)psramFound(),
                      (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return;
    }

    if (line.text == "/mic-info") {
        // What the driver actually resolved, not what we believe we asked for. A capture that is
        // constant low-frequency rumble at every gain has the signature of a port reading a pin
        // nothing is connected to, and the pin numbers are the only place that is visible.
        const auto c = M5.Mic.config();
        Serial.printf("[mic] enabled=%d recording=%d\n", M5.Mic.isEnabled(), M5.Mic.isRecording());
        Serial.printf("[mic] pin_data_in=%d bck=%d ws=%d mck=%d\n", c.pin_data_in, c.pin_bck,
                      c.pin_ws, c.pin_mck);
        Serial.printf("[mic] rate=%u stereo=%d over_sampling=%u magnification=%u noise=%u\n",
                      (unsigned)c.sample_rate, (int)c.stereo, (unsigned)c.over_sampling,
                      (unsigned)c.magnification, (unsigned)c.noise_filter_level);
        Serial.printf("[mic] i2s_port=%d dma_buf_len=%u dma_buf_count=%u use_adc=%d\n",
                      (int)c.i2s_port, (unsigned)c.dma_buf_len, (unsigned)c.dma_buf_count,
                      (int)c.use_adc);
        Serial.printf("[spk] enabled=%d\n", M5.Speaker.isEnabled());
        return;
    }

    if (line.text.rfind("/cal", 0) == 0) {
        unsigned seconds = 5;
        const std::size_t space = line.text.find(' ');
        if (space != std::string::npos) {
            const int parsed = std::atoi(line.text.c_str() + space + 1);
            if (parsed > 0 && parsed <= 15) seconds = static_cast<unsigned>(parsed);
        }
        cal_count = 0;
        cal_until_ms = millis() + seconds * 1000;
        Serial.printf("[cal] вимірюю %u с...\n", seconds);
        return;
    }

    if (line.text == "/faces") {
        startSelfTest();
        return;
    }
    // `on` / `off` as well as a bare toggle. **A script must be able to set a state, not flip
    // one** -- it cannot see the indicator, so a toggle leaves it guessing, and guessing wrong
    // means every scripted line afterwards is refused with `[busy] not idle`.
    if (line.text == "/mic on") {
        setActiveListening(true);
        return;
    }
    if (line.text == "/mic off") {
        setActiveListening(false);
        return;
    }
    if (line.text == "/mic") {
        // The same toggle the tap performs, reachable from a script.
        //
        // **Added because the device was not testable without it.** A scripted line is refused
        // unless the device is idle, and with active listening on in a room with any noise at all
        // the VAD opens windows continuously -- measured at `vad s=107 e=83`, windows lasting ten
        // seconds. A manual test then sends into `[busy] not idle` every time and reports on
        // something that never ran. A control a script cannot reach is a control that makes the
        // device untestable, whatever it does for a person.
        toggleActiveListening();
        return;
    }
    if (line.text == "/debug") {
        debug_line = !debug_line;
        Serial.printf("[debug] corner line %s\n", debug_line ? "on" : "off");
        render();
        return;
    }
    if (line.text == "/help") {
        Serial.println("  <text>   say something    /faces  cycle the six faces");
        Serial.println("  /debug   corner state line /probe  raw TCP path test");
        Serial.println("  /http    plain HTTP GET test");
        Serial.println("  /listen [s] capture and stream for s seconds (default 3)");
        Serial.println("  /loopback [s] record and play back locally — a diagnostic, sends nothing");
        Serial.println("  /mic [on|off] active listening; bare toggles, on/off sets");
        Serial.println("  /chat-on  show the conversation on the panel");
        Serial.println("  /chat-off return to the face");
        Serial.println("  /help    this");
        return;
    }

    if (self_test_running) {
        Serial.println("[busy] self-test running — line ignored");
        return;
    }
    if (!roboface::canStartTurn(state)) {
        Serial.printf("[busy] not idle (%s) — line ignored\n", roboface::toString(state));
        return;
    }
    if (!ws.sendTextIn(line.text.c_str())) {
        Serial.println("[error] no connection — line not sent");
        return;
    }
    transcript.startTurn(line.text);
    apply(roboface::DeviceEvent::kTurnStarted);
    (void)now_ms;
}

}  // namespace

void setup() {
    // Before anything claims memory. PSRAM is initialised by the startup code long before this
    // runs, so a zero here is a boot/build problem, and a non-zero that later reads zero is
    // something on this side destroying it.
    Serial.begin(115200);
    delay(400);
    Serial.printf("\n[boot] psram found=%d size=%u free=%u | internal free=%u\n",
                  (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    auto config = M5.config();
    //: The board's own microphone, asked for explicitly. The default depends on the build's board
    //: detection, and a capture path that silently reads a peripheral nobody enabled looks exactly
    //: like a working one -- right frame count, right level, no voice in it.
    config.internal_mic = true;
    config.internal_spk = true;
    M5.begin(config);

    Serial.begin(115200);
    delay(300);  // the USB CDC port takes a moment to enumerate after a reset

    Serial.println();
    Serial.printf("RoboFace firmware %s on %s\n", roboface::kFirmwareVersion, roboface::kBoard);
    Serial.println("type a line to say it; /faces to cycle the faces; /help for the rest.");

    M5.Display.setBrightness(SCREEN_BRIGHTNESS);

    // The renderer is the ONLY way this app draws a face (ROADMAP §v0.4 DoD). There is no
    // M5.Display drawing call anywhere below — the v0.3 banner that used to be here is gone.
    if (!renderer.begin()) {
        // A blank screen presented as a working one is worse than an admission. PSRAM is the only
        // thing that can fail here, and it fails silently otherwise.
        Serial.println("[face] FAILED to allocate the sprite — the screen will stay blank");
    }

    // The two sensors v2.4 adds. **Neither is required to boot**: a board variant without one is a
    // device that does not notice being shaken, not a device that refuses to start. The capability
    // flags that make that explicit on the wire arrive in v6.1.
    if (!imu.begin()) Serial.println("[imu] not present — no motion events");
    if (!proximity.begin()) Serial.println("[near] not present — the face will not wake to a hand");

    const uint32_t now_ms = millis();
    pollPower(now_ms, /*force=*/true);
    updateChrome(now_ms, false, roboface::ErrorCode::kUnknown);
    render();

    apply(roboface::DeviceEvent::kBooted);
    net.begin(WIFI_SSID, WIFI_PASSWORD, now_ms);
    if (!audio.begin(SPEAKER_VOLUME, MIC_GAIN)) {
        Serial.println("[audio] PSRAM backlog allocation FAILED — the device will be mute");
    } else {
        Serial.printf("[audio] backlog %u KB (psram free %u KB, internal free %u KB), volume %d\n",
                      static_cast<unsigned>(audio.backlogCapacity() / 1024),
                      static_cast<unsigned>(ESP.getFreePsram() / 1024),
                      static_cast<unsigned>(ESP.getFreeHeap() / 1024), SPEAKER_VOLUME);
    }

    // Step 1 of active listening: the recorder runs from boot with no window open. Nothing is sent
    // and nothing is decided -- the only question is whether an always-on recorder still captures
    // at full rate, which is the property every later step depends on.
    if (!audio.startMonitoring(&onCapturedFrame)) {
        Serial.println("[mic] monitoring did not start");
    }

    ws.onEvent(onSocketEvent);
    // A binary frame is `tts_audio`: it carries no envelope, and what gives it meaning is that the
    // server is speaking (`server_binary_meaning`). Taking the bus here rather than at the start of
    // the turn means a turn that never speaks never touches the microphone.
    // A binary frame is `tts_audio`: it carries no envelope, and what gives it meaning is that the
    // server is speaking (`server_binary_meaning`). The bus is taken here rather than at the start
    // of the turn, so a turn that never speaks never touches the microphone.
    ws.onBinary([](const uint8_t* payload, std::size_t length) {
        audio.startSpeaking();
        audio.write(payload, length);
    });
}

void loop() {
    M5.update();
    const uint32_t now_ms = millis();

    if (net.loop(now_ms)) {
        if (net.isUp()) {
            apply(roboface::DeviceEvent::kWifiUp);
            static bool socket_started = false;
            if (!socket_started) {
                ws.begin(SERVER_URL, DEVICE_ID, now_ms);
                socket_started = true;
            }
        } else {
            apply(roboface::DeviceEvent::kWifiLost);
        }
    }

    // Throttle the socket rather than throw audio away. The speaker paces the backlog and the
    // backlog paces the socket; TCP holds the rest on the server, where it costs nothing.
    if (!audio.isBackpressured()) ws.loop(now_ms);
    renderer.tick(now_ms);
    stepSelfTest(now_ms);

    while (Serial.available() > 0) {
        const roboface::LineReader::Line line = lines.feed(static_cast<char>(Serial.read()));
        if (line.complete) handleLine(line, now_ms);
    }

    // Repaint on change or while a fade runs, and never faster than kMinPushIntervalMs. The face
    // is redrawn only on a state change -- which is why show() clears the face area alone -- and
    // chrome is composited over it here.
    // Touch before audio: a release must reach `endListening` promptly, or the last frames of an
    // utterance keep arriving after the person has let go.
    //
    // `M5.update()` has already refreshed the panel this loop. `getDetail().isPressed()` is the
    // level; `PushToTalk` turns it into the edges that open and close exactly one window.
    // A voice and a finger go through the same beginListening/endListening, so the two triggers
    // cannot drift apart.
    switch (pending_vad) {
        case roboface::VadEvent::kSpeechStarted:
            pending_vad = roboface::VadEvent::kNone;
            listen_until_ms = 0;
            // **A refused window is not a window.** Without this the endpointer stays convinced
            // speech is still going -- it never fires again, and the device is deaf for the rest
            // of the session. Measured: one refusal at boot, before the socket was up, was enough.
            if (!beginListening()) endpointer.reset();
            break;
        case roboface::VadEvent::kSpeechEnded:
            pending_vad = roboface::VadEvent::kNone;
            endListening("vad-end");
            break;
        default:
            pending_vad = roboface::VadEvent::kNone;
            break;
    }

    // **The affection half of the same finger** (v2.4). `PushToTalk` above decides what the touch
    // does to the microphone; `TouchGestures` decides what it meant to the character. Both are fed
    // the same panel state in the same loop, so they cannot disagree about whether a finger is down.
    // **Mute is the two-finger tap from v2.4**, which is what DEVICE_UI §Input always specified.
    // Counted on the edge -- a second finger arriving while the first is down -- rather than on
    // release, because the panel reports the count only while both are present and a release has
    // already lost it.
    {
        const uint8_t fingers = M5.Touch.getCount();
        if (fingers >= 2 && fingers_last < 2) toggleActiveListening();
        fingers_last = fingers;
    }

    {
        const auto detail = M5.Touch.getDetail();
        const roboface::TouchSample sample{detail.isPressed(), detail.x, detail.y, now_ms};
        const roboface::TouchResult touched = gestures.feed(sample, audio.isListening());
        if (touched.gesture != roboface::TouchGesture::kNone) {
            onTouchGesture(touched, now_ms);
        }
    }

    switch (ptt.update(M5.Touch.getDetail().isPressed(), now_ms)) {
        case roboface::PttEvent::kStarted:
            // A timed `/listen` window and a finger must not both own the microphone.
            listen_until_ms = 0;
            beginListening();
            break;
        case roboface::PttEvent::kStopped:
            endListening("ptt");
            break;
        case roboface::PttEvent::kTapped: {
            // **The tap is affection again** (v2.4). DEVICE_UI §Input always said so and recorded
            // the single-tap mute as temporary -- *"the single tap belongs to the affection reflex;
            // mute moves to two fingers when the reflexes land"*. They landed here, so the
            // stop-gap goes: `TouchGestures` above turns this same tap into a tickle.
            //
            // Mute is the **two-finger tap** now, and `/mic on|off` stays -- it is what makes the
            // device scriptable, and every manual test since v2.3 depends on it.
            break;
        }
            // Deliberately nothing on the wire. DEVICE_UI gives press-and-hold to PTT; a tap is
            // reserved, and treating it as a very short utterance would send the server a window
            // with nothing in it.

        case roboface::PttEvent::kNone:
            break;
    }

    // Before the screen: audio starving is audible and a late repaint is not.
    now_ms_for_log = now_ms;
    audio.tick(now_ms);

    // The mouth's signal, from the speaker rather than the microphone. Fed every loop and not only
    // while replying: the renderer decides whether to use it, and a level that stopped arriving
    // would leave the mouth frozen at whatever it last heard.
    // The two sensors, each at its own rate and each deciding nothing here.
    if (const roboface::Motion motion = imu.tick(now_ms); motion != roboface::Motion::kNone) {
        onMotion(motion, now_ms);
    }
    if (const roboface::Presence presence = proximity.tick(now_ms);
        presence != roboface::Presence::kNone) {
        onPresence(presence, now_ms);
    }

    renderer.setAudioLevel(audio.outputLevel());
    // The **fact**, next to the server's permission. `v2.1.2` is the reason both are needed: the
    // server's idea of when a reply ends runs seconds ahead of the speaker, because the device is
    // still draining audio the server finished sending.
    renderer.setPlaying(audio.isSpeaking());
    if (audio.isSpeaking()) needs_push = true;

    // The reply is over now: the last sample has left the speaker.
    if (reply_end_pending && !audio.isSpeaking()) {
        reply_end_pending = false;
        apply(roboface::DeviceEvent::kTurnEnded);
    }

    // Hearing came back after the device finished speaking. Clear the endpointer rather than
    // letting it carry the reply across: the silence during playback is not part of anyone's
    // pause, and the tail of the last sentence is not the start of the next one.
    if (audio.takeMonitorResumed()) {
        endpointer.reset();
        pending_vad = roboface::VadEvent::kNone;
    }

    // The face animates on its own now: blinking, breathing, drifting. The renderer says whether
    // anything actually moved, so a settled face still costs nothing.
    if (renderer.isAnimating()) needs_push = true;

    // A live meter needs live repaints, but **not at the panel's full rate**. Marking the sprite
    // dirty every loop made the 33 ms cap fire constantly, and a full 153 KB sprite push at 30 Hz
    // left the loop unable to service the microphone within its 40 ms of buffering: capture
    // returned 47% of every window, at any window length, and the missing half made recognition
    // return nothing at all.
    //
    // Ten frames a second is a smooth-looking meter and a quarter of the cost. The audio is the
    // product; the meter is a courtesy.
    if (audio.isListening() && now_ms - last_meter_ms >= kMeterIntervalMs) {
        last_meter_ms = now_ms;
        needs_push = true;
    }
    // `millis()`, **not** the loop's `now_ms`. `now_ms` is read at the top of the iteration and
    // `listen_opened_ms` is stamped later in the same one, so `now_ms - listen_opened_ms` is
    // negative -- and on a uint32_t that is about four billion, which is always past any cap.
    //
    // Measured: every window closed 53 ms after it opened, which is why each one carried a single
    // frame, never reported an end, and reopened immediately. The counter said "closed by cap" and
    // was telling the truth; the cap was simply arriving four billion milliseconds early.
    const uint32_t window_open_ms = listen_opened_ms == 0 ? 0 : millis() - listen_opened_ms;
    if (audio.isListening() && listen_opened_ms != 0 && window_open_ms >= kMaxWindowMs) {
        listen_until_ms = 0;
        endListening("cap");
    }

    if (cal_until_ms != 0 && now_ms >= cal_until_ms) {
        cal_until_ms = 0;
        uint16_t zc_median = 0;
        if (cal_count > 0) {
            // The crossing count of the frames that were actually loud -- a quiet frame's crossings
            // say nothing about whether speech crosses zero often.
            std::size_t loud = 0;
            for (std::size_t i = 0; i < cal_count; ++i) if (cal_zc[i] > zc_median) zc_median = cal_zc[i];
            (void)loud;
        }
        const uint8_t p10 = percentileOf(cal_peak, cal_count, 10);
        const uint8_t p50 = percentileOf(cal_peak, cal_count, 50);
        const uint8_t p90 = percentileOf(cal_peak, cal_count, 90);
        const uint8_t p99 = percentileOf(cal_peak, cal_count, 99);
        Serial.printf("[cal] кадрів=%u · peak p10=%u%% p50=%u%% p90=%u%% max=%u%% · zc max=%u\n",
                      static_cast<unsigned>(cal_count), static_cast<unsigned>(p10),
                      static_cast<unsigned>(p50), static_cast<unsigned>(p90),
                      static_cast<unsigned>(p99), static_cast<unsigned>(zc_median));
        return;
    }

    if (listen_until_ms != 0 && now_ms >= listen_until_ms) {
        listen_until_ms = 0;
        endListening("timer");
    }
    if (loopback_until_ms != 0 && now_ms >= loopback_until_ms) {
        loopback_until_ms = 0;
        Serial.printf("[loopback] captured %u frames (%u ms), peak %d%% — playing back\n",
                      static_cast<unsigned>(audio.tally().frames()),
                      static_cast<unsigned>(audio.tally().durationMs()),
                      static_cast<int>(audio.peakSeen() * 100.0f));
        audio.playBacklog();
    }

    pollPower(now_ms, /*force=*/false);
    updateChrome(now_ms, fault_active, fault_code);
    // Three rates, because the three states have different things moving on the screen: the idle
    // loop when nothing is happening, the level meter while listening, and -- until v2.3 adds
    // lip-sync -- nothing at all while speaking.
    const uint32_t push_interval = audio.isListening()  ? kListeningPushIntervalMs
                                   : audio.isSpeaking() ? kBusyPushIntervalMs
                                                        : kMinPushIntervalMs;
    if (needs_push && now_ms - last_push_ms >= push_interval) {
        if (console.isOn()) console_view.draw(renderer.canvas(), transcript);
        chrome_view.draw(renderer.canvas(), chrome, audio.inputLevel());
        renderer.push();
        last_push_ms = now_ms;
        needs_push = false;
    }

    if (now_ms - last_status_ms >= kStatusIntervalMs) {
        last_status_ms = now_ms;
        const auto mouth_stats = renderer.takeMouthStats();
        Serial.printf(
            "[status] %s · link %s %s · ws %s · batt %d%%%s · fps=%lu mic=%lu cap=%lu · mon=%d "
            "vad s=%lu e=%lu peak=%d%% zc=%u · audio %s buf=%u q=%u ref=%u drop=%u · "
            "mouth=%lu lvl=%d..%d%% · up %lus\n",
            roboface::toString(state), net.isUp() ? "up" : "down", net.ipAddress(),
            ws.isConnected() ? "connected" : "disconnected", battery_percent,
            battery_charging ? " (charging)" : "",
            // **Both numbers, side by side.** Either alone hides the trade: a face at 18 FPS that
            // cost a third of the audio looks healthy in one and is a regression in the other. In
            // v1 exactly that went unnoticed three separate times.
            static_cast<unsigned long>((renderer.framesPushed() - frames_at_status) * 1000u /
                                       kStatusIntervalMs),
            // `mic` is the recorder's own rate and should sit at ~50 whenever the microphone is
            // running, window or no window. It is the number that separates "the room was quiet"
            // from "the recorder is starved" -- two situations that look identical from `cap`,
            // and that need opposite fixes.
            static_cast<unsigned long>((frames_captured - captured_at_status) * 1000u /
                                       kStatusIntervalMs),
            // Saturating, because the capture tally resets at every window: a stale baseline
            // larger than the current count would underflow into four billion, which is exactly
            // the mistake the listening-window cap made earlier in this project. Unsigned
            // subtraction against a counter someone else resets is worth distrusting on sight.
            static_cast<unsigned long>(
                audio.tally().frames() > capture_at_status
                    ? (audio.tally().frames() - capture_at_status) * 1000u / kStatusIntervalMs
                    : 0u),
            static_cast<int>(audio.isMonitoring()),
            static_cast<unsigned long>(vad_starts), static_cast<unsigned long>(vad_ends),
            static_cast<int>(peak_recent * 100.0f), static_cast<unsigned>(crossings_recent),
            audio.isSpeaking() ? "on" : "off", static_cast<unsigned>(audio.buffered()),
            static_cast<unsigned>(audio.bytesQueued()),
            static_cast<unsigned>(audio.chunksRefused()),
            static_cast<unsigned>(audio.bytesDropped()),
            // The mouth, as two facts rather than a stream of shape changes: how often it actually
            // moved, and the range of levels it was given. Together they separate the two ways a
            // mouth stops -- no signal (`lvl=0..0`) from a signal that never varies (`lvl=88..99`),
            // which is a level pinned at the top and a mouth stuck open.
            static_cast<unsigned long>(mouth_stats.changes),
            static_cast<int>(mouth_stats.level_min * 100.0f),
            static_cast<int>(mouth_stats.level_max * 100.0f),
            static_cast<unsigned long>(now_ms / 1000));
        peak_recent = 0.0f;
        crossings_recent = 0;
        frames_at_status = renderer.framesPushed();
        capture_at_status = audio.tally().frames();
        captured_at_status = frames_captured;
    }

    delay(5);
}

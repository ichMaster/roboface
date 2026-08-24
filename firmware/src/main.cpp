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
#include "app/stub_renderer.h"
#include "app/ws.h"
#include "config.h"
#include "pure/chrome.h"
#include "pure/console.h"
#include "pure/transcript.h"
#include "pure/line_reader.h"
#include "pure/state.h"
#include "pure/version.h"
#include "pure/ws_protocol.h"
#include "pure/ws_url.h"

namespace {

app::Net net;
app::Ws ws;
app::StubRenderer renderer;
app::ChromeView chrome_view;
app::ConsoleView console_view;
app::AudioIo audio;
roboface::Chrome chrome;
roboface::LineReader lines;
roboface::DeviceState state = roboface::DeviceState::kBoot;

constexpr uint32_t kStatusIntervalMs = 10000;

// Nothing on this screen changes faster than DEVICE_UI's 120 ms chrome fade -- about 8 Hz. 30 Hz is
// generous for a face that is currently static, and it is the difference between the loop serving
// the network and the loop being one long sprite push: 320x240x16 is 153 KB, so the unconditional
// push in every 5 ms iteration implied ~30 MB/s on a bus that carries about 5.
constexpr uint32_t kMinPushIntervalMs = 33;
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

// The serial chat console (v0.5). It borrows the screen the way the self-test does, and gives back
// the state it took -- the discipline lives in `pure/console.h` where a host test proves it is
// total over the state enum.
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

void render() {
    // The console borrows the face area; chrome keeps its bands either way, because link and
    // battery are facts and hiding them would hide a dropped link exactly when it matters.
    if (console.isOn()) {
        console_view.draw(renderer.canvas(), transcript);
    } else {
        renderer.show(state);
    }
    chrome_view.draw(renderer.canvas(), chrome);
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
    const roboface::Transition result = roboface::transition(state, event);
    if (!result.accepted) return;

    state = result.next;
    Serial.printf("\n[state] %s\n", roboface::toString(state));
    render();
    needs_push = true;
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
            // Speech that has been superseded is worse than silence: it answers a question
            // nobody is still asking.
            audio.abort();
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

    if (line.text == "/faces") {
        startSelfTest();
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
    auto config = M5.config();
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

    const uint32_t now_ms = millis();
    pollPower(now_ms, /*force=*/true);
    updateChrome(now_ms, false, roboface::ErrorCode::kUnknown);
    render();

    apply(roboface::DeviceEvent::kBooted);
    net.begin(WIFI_SSID, WIFI_PASSWORD, now_ms);
    audio.begin(SPEAKER_VOLUME);

    ws.onEvent(onSocketEvent);
    // A binary frame is `tts_audio`: it carries no envelope, and what gives it meaning is that the
    // server is speaking (`server_binary_meaning`). Taking the bus here rather than at the start of
    // the turn means a turn that never speaks never touches the microphone.
    ws.onBinary([](const uint8_t* payload, std::size_t length) {
        audio.startSpeaking();
        std::size_t offset = 0;
        // Honour backpressure rather than dropping the tail. A buffer that discarded what did not
        // fit would lose a few milliseconds of speech exactly when the network is struggling, and
        // the symptom -- occasionally clipped words under load -- is indistinguishable from a bad
        // voice model. Bounded, because this runs in the socket callback and must return.
        for (int attempt = 0; attempt < 64 && offset < length; ++attempt) {
            offset += audio.write(payload + offset, length - offset);
            if (offset < length) audio.tick();
        }
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

    ws.loop(now_ms);
    renderer.tick(now_ms);
    stepSelfTest(now_ms);

    while (Serial.available() > 0) {
        const roboface::LineReader::Line line = lines.feed(static_cast<char>(Serial.read()));
        if (line.complete) handleLine(line, now_ms);
    }

    // Repaint on change or while a fade runs, and never faster than kMinPushIntervalMs. The face
    // is redrawn only on a state change -- which is why show() clears the face area alone -- and
    // chrome is composited over it here.
    // Before the screen: audio starving is audible and a late repaint is not.
    audio.tick();

    pollPower(now_ms, /*force=*/false);
    updateChrome(now_ms, fault_active, fault_code);
    if (needs_push && now_ms - last_push_ms >= kMinPushIntervalMs) {
        if (console.isOn()) console_view.draw(renderer.canvas(), transcript);
        chrome_view.draw(renderer.canvas(), chrome);
        renderer.push();
        last_push_ms = now_ms;
        needs_push = false;
    }

    if (now_ms - last_status_ms >= kStatusIntervalMs) {
        last_status_ms = now_ms;
        Serial.printf("[status] %s · link %s %s · ws %s · batt %d%%%s · up %lus\n",
                      roboface::toString(state), net.isUp() ? "up" : "down", net.ipAddress(),
                      ws.isConnected() ? "connected" : "disconnected", battery_percent,
                      battery_charging ? " (charging)" : "",
                      static_cast<unsigned long>(now_ms / 1000));
    }

    delay(5);
}

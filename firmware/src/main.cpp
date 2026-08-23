// RoboFace firmware — entry point and the serial debug channel.
//
// v0.3's payoff: a line typed into `pio device monitor` becomes a `text_in`, and the model's reply
// prints back **as it arrives**. USB serial is the debug channel and the only turn source until v1
// brings the microphone (ROADMAP §v0.3) — the same trick that gets a testable loop before audio
// exists.
//
// This file is wiring and stays wiring. Every decision it appears to make is delegated: the state
// machine says what a state change means, `roboface::LineReader` says where a line ends,
// `roboface::ws_protocol` says what a frame is. What is left here is the order things happen in,
// which is the one thing a host test cannot check and a person watching a serial monitor can.

#include <M5Unified.h>

#include "app/net.h"
#include "app/ws.h"
#include "config.h"
#include "pure/line_reader.h"
#include "pure/state.h"
#include "pure/version.h"
#include "pure/ws_protocol.h"

namespace {

app::Net net;
app::Ws ws;
roboface::LineReader lines;
roboface::DeviceState state = roboface::DeviceState::kBoot;

// A status line often enough to tell a wedged device from a quiet one, rarely enough that it does
// not bury the conversation it is printed alongside.
constexpr uint32_t kStatusIntervalMs = 10000;
uint32_t last_status_ms = 0;
bool reply_in_progress = false;

void apply(roboface::DeviceEvent event) {
    const roboface::Transition result = roboface::transition(state, event);
    if (!result.accepted) return;

    state = result.next;
    Serial.printf("\n[state] %s\n", roboface::toString(state));
}

void onSocketEvent(app::Ws::Event event, const roboface::ServerFrame& frame) {
    switch (event) {
        case app::Ws::Event::kConnected:
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
            // Already logged by `ws` with its reason. Not a device fault: a frame this build does
            // not handle is the server being newer, not the server being broken.
            return;

        case app::Ws::Event::kFrame:
            break;
    }

    switch (frame.result) {
        case roboface::ParseResult::kReply:
            if (frame.final) {
                // The turn is over. The terminal frame carries no text -- it closes, it does not
                // repeat.
                if (reply_in_progress) Serial.println();
                reply_in_progress = false;
                apply(roboface::DeviceEvent::kTurnEnded);
                return;
            }
            // **Written inside the receive path, not accumulated.** A buffered implementation
            // would satisfy the DoD's wording and throw away the property v0.2 exists to provide:
            // the first words appear while the last are still being generated.
            if (!reply_in_progress) {
                apply(roboface::DeviceEvent::kReplyStarted);
                Serial.print("  ");
                reply_in_progress = true;
            }
            Serial.print(frame.text.c_str());
            return;

        case roboface::ParseResult::kError:
            if (reply_in_progress) {
                Serial.println();
                reply_in_progress = false;
            }
            // The enumerated code verbatim: it is the one string worth typing into a search, and
            // features/DEVICE_UI.md applies the same reasoning to the fault screen v0.4 adds.
            Serial.printf("\n[error] %s: %s\n", roboface::toString(frame.code), frame.msg.c_str());
            apply(roboface::DeviceEvent::kFault);
            return;

        case roboface::ParseResult::kPong:
        default:
            return;
    }
}

void printStatus(uint32_t now_ms) {
    Serial.printf("[status] %s · link %s %s · ws %s · up %lus\n", roboface::toString(state),
                  net.isUp() ? "up" : "down", net.ipAddress(),
                  ws.isConnected() ? "connected" : "disconnected",
                  static_cast<unsigned long>(now_ms / 1000));
}

}  // namespace

void setup() {
    auto config = M5.config();
    M5.begin(config);

    Serial.begin(115200);
    // The USB CDC port takes a moment to enumerate after a reset; without this the first lines go
    // into a port nobody is attached to yet, which looks exactly like a board that failed to boot.
    delay(300);

    Serial.println();
    Serial.printf("RoboFace firmware %s on %s\n", roboface::kFirmwareVersion, roboface::kBoard);
    Serial.println("type a line and press return; it becomes a text_in.");

    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.printf("RoboFace\n%s\n", roboface::kFirmwareVersion);

    const uint32_t now_ms = millis();
    apply(roboface::DeviceEvent::kBooted);

    net.begin(WIFI_SSID, WIFI_PASSWORD, now_ms);
    ws.onEvent(onSocketEvent);
}

void loop() {
    M5.update();
    const uint32_t now_ms = millis();

    if (net.loop(now_ms)) {
        if (net.isUp()) {
            apply(roboface::DeviceEvent::kWifiUp);
            // The socket is only worth opening once there is a link to open it over. Starting it
            // in setup() would spend the whole association window failing to connect.
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

    while (Serial.available() > 0) {
        const roboface::LineReader::Line line = lines.feed(static_cast<char>(Serial.read()));
        if (!line.complete) continue;

        if (line.truncated) {
            Serial.printf("[warn] line truncated to %u characters\n",
                          static_cast<unsigned>(line.text.size()));
        }
        if (!roboface::canStartTurn(state)) {
            Serial.printf("[busy] not idle (%s) — line ignored\n", roboface::toString(state));
            continue;
        }
        if (!ws.sendTextIn(line.text.c_str())) {
            Serial.println("[error] no connection — line not sent");
            continue;
        }
        apply(roboface::DeviceEvent::kTurnStarted);
    }

    if (now_ms - last_status_ms >= kStatusIntervalMs) {
        last_status_ms = now_ms;
        printStatus(now_ms);
    }

    delay(5);
}

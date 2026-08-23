// The device state machine.
//
// ARCHITECTURE.md §Device states: `boot → wifi_connecting → idle → listening → thinking →
// replying → idle`, with `offline` and `error` reachable **from anywhere**. The state drives the
// face (from v0.4); the device holds no persona logic and decides no emotion — it renders what it
// is given and reports what it senses.
//
// `idle` means **ready to be spoken to** — WiFi up *and* the socket open. That distinction is why
// `wifi_connecting` exits on `kSocketUp` rather than on `kWifiUp`: the window between the link
// returning and the socket reopening is where a reconnect spends most of its time, and a device
// showing `idle` through it would be inviting a person to type into nothing.
//
// `transition()` is **total** over (state, event): every pair either produces a next state or is
// explicitly rejected. That totality is the point. A machine with holes in it does not crash — it
// quietly sits in the wrong state, which on a device with no keyboard and no stack trace is the
// hardest possible failure to diagnose. Here, an unhandled pair is a test failure on a laptop.

#pragma once

namespace roboface {

enum class DeviceState {
    kBoot,
    kWifiConnecting,
    kIdle,
    kListening,  // declared now, driven from v1 -- see the note below
    kThinking,
    kReplying,
    kOffline,
    kError,
};

// `kListening` is part of the specified state set even though v0.3 never enters it: v1 adds the
// microphone and the push-to-talk that do. Declaring it now means v1 extends the machine; leaving
// it out would mean rewriting one that other code already depends on the shape of.

enum class DeviceEvent {
    kBooted,
    kWifiUp,
    kWifiLost,
    kSocketUp,
    kSocketLost,
    kTurnStarted,   // a text_in was sent -- from v1, speech ends and recognition begins
    kReplyStarted,  // the first reply delta arrived
    kTurnEnded,     // the terminal reply frame
    kFault,         // an enumerated error{code} from the server, or a local one
    kFaultCleared,
    kListenStarted,  // v1
    kListenStopped,  // v1
};

inline const char* toString(DeviceState state) {
    switch (state) {
        case DeviceState::kBoot: return "boot";
        case DeviceState::kWifiConnecting: return "wifi_connecting";
        case DeviceState::kIdle: return "idle";
        case DeviceState::kListening: return "listening";
        case DeviceState::kThinking: return "thinking";
        case DeviceState::kReplying: return "replying";
        case DeviceState::kOffline: return "offline";
        case DeviceState::kError: return "error";
    }
    return "";
}

// The outcome of offering an event to a state.
//
// `accepted == false` is a *defined* answer, not a gap: "a reply cannot start while offline" is
// something the machine knows, and saying so is different from never having considered it.
struct Transition {
    DeviceState next;
    bool accepted;
};

inline constexpr Transition rejected(DeviceState current) { return Transition{current, false}; }
inline constexpr Transition accepted(DeviceState next) { return Transition{next, true}; }

inline constexpr Transition transition(DeviceState current, DeviceEvent event) {
    // Reachable from anywhere, so they are answered before the per-state table. ARCHITECTURE is
    // explicit that `offline` and `error` are not states you can only fall into from some places:
    // WiFi can vanish mid-reply, and a fault can arrive at any moment.
    switch (event) {
        case DeviceEvent::kFault:
            return accepted(DeviceState::kError);
        case DeviceEvent::kWifiLost:
            // Already offline is not a transition, but it is not an error either -- a supervisor
            // re-reporting a link that is still down must not look like a fault.
            return current == DeviceState::kOffline ? rejected(current)
                                                    : accepted(DeviceState::kOffline);
        default:
            break;
    }

    switch (current) {
        case DeviceState::kBoot:
            if (event == DeviceEvent::kBooted) return accepted(DeviceState::kWifiConnecting);
            return rejected(current);

        case DeviceState::kWifiConnecting:
            // **The socket, not the link.** `idle` means "ready to be spoken to", and a device
            // whose WiFi is up but whose socket is not cannot be. Exiting on kWifiUp would show
            // `idle` during the window where a typed line has nowhere to go -- which is exactly
            // the window a reconnect spends most of its time in.
            if (event == DeviceEvent::kSocketUp) return accepted(DeviceState::kIdle);
            // The link coming up is progress, not readiness: stay, and wait for the socket.
            if (event == DeviceEvent::kWifiUp) return rejected(current);
            return rejected(current);

        case DeviceState::kIdle:
            if (event == DeviceEvent::kTurnStarted) return accepted(DeviceState::kThinking);
            if (event == DeviceEvent::kListenStarted) return accepted(DeviceState::kListening);
            if (event == DeviceEvent::kSocketLost) return accepted(DeviceState::kOffline);
            return rejected(current);

        case DeviceState::kListening:
            if (event == DeviceEvent::kListenStopped) return accepted(DeviceState::kThinking);
            if (event == DeviceEvent::kTurnEnded) return accepted(DeviceState::kIdle);
            if (event == DeviceEvent::kSocketLost) return accepted(DeviceState::kOffline);
            return rejected(current);

        case DeviceState::kThinking:
            if (event == DeviceEvent::kReplyStarted) return accepted(DeviceState::kReplying);
            // A turn that produced nothing -- the silent model the server ends cleanly rather
            // than as an error. The device must return to idle, not sit in `thinking` forever.
            if (event == DeviceEvent::kTurnEnded) return accepted(DeviceState::kIdle);
            if (event == DeviceEvent::kSocketLost) return accepted(DeviceState::kOffline);
            return rejected(current);

        case DeviceState::kReplying:
            if (event == DeviceEvent::kTurnEnded) return accepted(DeviceState::kIdle);
            // More deltas while already replying: the common case, and not a state change.
            if (event == DeviceEvent::kReplyStarted) return rejected(current);
            if (event == DeviceEvent::kSocketLost) return accepted(DeviceState::kOffline);
            return rejected(current);

        case DeviceState::kOffline:
            // The link is back, but the socket is not: `wifi_connecting` rather than `idle`,
            // because a device that claims to be idle while it cannot reach the server is lying
            // to the person looking at it.
            if (event == DeviceEvent::kWifiUp) return accepted(DeviceState::kWifiConnecting);
            if (event == DeviceEvent::kSocketUp) return accepted(DeviceState::kIdle);
            return rejected(current);

        case DeviceState::kError:
            if (event == DeviceEvent::kFaultCleared) return accepted(DeviceState::kIdle);
            if (event == DeviceEvent::kSocketUp) return accepted(DeviceState::kIdle);
            return rejected(current);
    }

    return rejected(current);
}

// Whether the device can start a turn right now. Kept here rather than at the call site so the
// answer is testable and there is only one of it.
inline constexpr bool canStartTurn(DeviceState state) { return state == DeviceState::kIdle; }

}  // namespace roboface

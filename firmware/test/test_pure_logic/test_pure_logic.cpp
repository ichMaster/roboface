// Host tests for the three pure modules: the state machine, backoff, and the line reader.
//
// Between them these carry every decision the firmware makes. None of it needs a board, which is
// the whole reason for the pure/glue split — what is left in `app/` is wiring, and wiring is what
// a compile and a smoke check can cover.

#include <unity.h>

#include <set>
#include <string>
#include <vector>

#include "pure/backoff.h"
#include "pure/line_reader.h"
#include "pure/state.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------------------

static const DeviceState kAllStates[] = {
    DeviceState::kBoot,     DeviceState::kWifiConnecting, DeviceState::kIdle,
    DeviceState::kListening, DeviceState::kThinking,      DeviceState::kReplying,
    DeviceState::kOffline,  DeviceState::kError,
};

static const DeviceEvent kAllEvents[] = {
    DeviceEvent::kBooted,        DeviceEvent::kWifiUp,        DeviceEvent::kWifiLost,
    DeviceEvent::kSocketUp,      DeviceEvent::kSocketLost,    DeviceEvent::kTurnStarted,
    DeviceEvent::kReplyStarted,  DeviceEvent::kTurnEnded,     DeviceEvent::kFault,
    DeviceEvent::kFaultCleared,  DeviceEvent::kListenStarted, DeviceEvent::kListenStopped,
};

//: The transition the server-driven close relies on. v1.4 lets the *recogniser* end an utterance:
//: the device receives `asr`, closes its window, and must land in `kThinking` -- the same place a
//: released finger and an elapsed end-pause land it. Three routes into one transition, which is
//: exactly why it is worth pinning that the transition exists and is accepted.
static void test_a_window_closed_by_the_server_lands_in_thinking() {
    const auto closed = roboface::transition(DeviceState::kListening, DeviceEvent::kListenStopped);
    TEST_ASSERT_TRUE(closed.accepted);
    TEST_ASSERT_TRUE(closed.next == DeviceState::kThinking);
}

static void test_the_transition_table_is_total() {
    // Every (state, event) pair is answered — accepted with a next state, or explicitly
    // rejected. A machine with holes does not crash; it sits in the wrong state, which on a
    // device with no keyboard is the hardest failure there is to diagnose.
    for (const auto state : kAllStates) {
        for (const auto event : kAllEvents) {
            const Transition result = transition(state, event);
            if (!result.accepted) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(state), static_cast<int>(result.next),
                                              "a rejected transition must not move the state");
            }
        }
    }
}

static void test_every_state_has_a_name() {
    std::set<std::string> names;
    for (const auto state : kAllStates) {
        const std::string name = toString(state);
        TEST_ASSERT_FALSE(name.empty());
        names.insert(name);
    }
    TEST_ASSERT_EQUAL_UINT(8u, static_cast<unsigned>(names.size()));
}

static void test_the_state_names_are_the_specified_ones() {
    // ARCHITECTURE §Device states names them exactly; the server logs and (from v0.4) the screen
    // both use these strings.
    const std::set<std::string> expected{"boot",     "wifi_connecting", "idle",    "listening",
                                         "thinking", "replying",        "offline", "error"};
    std::set<std::string> actual;
    for (const auto state : kAllStates) actual.insert(toString(state));

    TEST_ASSERT_TRUE(actual == expected);
}

static void test_the_happy_path_runs_boot_to_idle_and_back() {
    DeviceState state = DeviceState::kBoot;
    state = transition(state, DeviceEvent::kBooted).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWifiConnecting), static_cast<int>(state));
    // The link, then the socket. `idle` means ready to be spoken to, so it waits for both.
    state = transition(state, DeviceEvent::kWifiUp).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWifiConnecting), static_cast<int>(state));
    state = transition(state, DeviceEvent::kSocketUp).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kIdle), static_cast<int>(state));
    state = transition(state, DeviceEvent::kTurnStarted).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kThinking), static_cast<int>(state));
    state = transition(state, DeviceEvent::kReplyStarted).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kReplying), static_cast<int>(state));
    state = transition(state, DeviceEvent::kTurnEnded).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kIdle), static_cast<int>(state));
}

static void test_a_fault_is_reachable_from_every_state() {
    // ARCHITECTURE: `error` is reachable from anywhere. A fault that arrived mid-reply and was
    // ignored would leave the device looking like it was still answering.
    for (const auto state : kAllStates) {
        const Transition result = transition(state, DeviceEvent::kFault);
        TEST_ASSERT_TRUE(result.accepted);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kError), static_cast<int>(result.next));
    }
}

static void test_losing_wifi_is_reachable_from_every_state_except_offline() {
    for (const auto state : kAllStates) {
        const Transition result = transition(state, DeviceEvent::kWifiLost);
        if (state == DeviceState::kOffline) {
            // Re-reporting a link that is still down must not look like a fresh event.
            TEST_ASSERT_FALSE(result.accepted);
            continue;
        }
        TEST_ASSERT_TRUE(result.accepted);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kOffline),
                              static_cast<int>(result.next));
    }
}

static void test_recovery_returns_to_idle() {
    // The DoD's second clause, minus the hardware: offline, then back.
    DeviceState state = transition(DeviceState::kReplying, DeviceEvent::kWifiLost).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kOffline), static_cast<int>(state));

    state = transition(state, DeviceEvent::kWifiUp).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWifiConnecting), static_cast<int>(state));

    state = transition(state, DeviceEvent::kSocketUp).next;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kIdle), static_cast<int>(state));
}

static void test_offline_does_not_claim_to_be_idle_before_the_socket_is_back() {
    // WiFi returning is not the server returning. A device that showed `idle` while it could not
    // reach the server would be inviting a person to type into nothing.
    const Transition from_offline = transition(DeviceState::kOffline, DeviceEvent::kWifiUp);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWifiConnecting),
                          static_cast<int>(from_offline.next));

    // And the same one step later: the link is up, the socket is not, so it stays put.
    const Transition still_connecting =
        transition(DeviceState::kWifiConnecting, DeviceEvent::kWifiUp);
    TEST_ASSERT_FALSE(still_connecting.accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWifiConnecting),
                          static_cast<int>(still_connecting.next));
}

static void test_a_turn_that_says_nothing_still_returns_to_idle() {
    // The silent model: the server ends the turn cleanly with no deltas. Without this the device
    // would sit in `thinking` forever waiting for a reply that already finished.
    const Transition result = transition(DeviceState::kThinking, DeviceEvent::kTurnEnded);

    TEST_ASSERT_TRUE(result.accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kIdle), static_cast<int>(result.next));
}

static void test_more_deltas_while_replying_is_not_a_state_change() {
    const Transition result = transition(DeviceState::kReplying, DeviceEvent::kReplyStarted);

    TEST_ASSERT_FALSE(result.accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kReplying), static_cast<int>(result.next));
}

static void test_the_server_may_speak_first() {
    // **v2.4 gave the server a reason to start a turn the device never asked for.** It answers an
    // `event{}` -- a device stroked or shaken while idle gets a spoken reaction nobody requested --
    // and until this transition existed the reply was thrown away with
    // `[warn] reply outside a turn (idle) -- ignored`. The guard was right for v1, where every turn
    // began with a person holding the button; the product changed under it.
    const Transition result = transition(DeviceState::kIdle, DeviceEvent::kReplyStarted);

    TEST_ASSERT_TRUE(result.accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kReplying), static_cast<int>(result.next));
}

static void test_a_turn_cannot_start_unless_idle() {
    for (const auto state : kAllStates) {
        TEST_ASSERT_EQUAL_INT(state == DeviceState::kIdle ? 1 : 0, canStartTurn(state) ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------------------

static void test_backoff_grows_exponentially() {
    Backoff backoff(100, 10000, 2);

    TEST_ASSERT_EQUAL_UINT32(100u, backoff.nextDelayMs());
    TEST_ASSERT_EQUAL_UINT32(200u, backoff.nextDelayMs());
    TEST_ASSERT_EQUAL_UINT32(400u, backoff.nextDelayMs());
    TEST_ASSERT_EQUAL_UINT32(800u, backoff.nextDelayMs());
}

static void test_backoff_is_capped_at_the_ceiling() {
    // Unbounded doubling means a device that lost its router at 3 a.m. waits nine hours by
    // breakfast. The ceiling is what makes "reconnects on its own" true on a human timescale.
    Backoff backoff(100, 1000, 2);

    for (int i = 0; i < 20; ++i) backoff.nextDelayMs();

    TEST_ASSERT_EQUAL_UINT32(1000u, backoff.peekDelayMs());
    TEST_ASSERT_EQUAL_UINT32(1000u, backoff.nextDelayMs());
}

static void test_backoff_resets_after_a_success() {
    // Without this a device that flaps once an hour keeps climbing all day, and eventually meets
    // a two-second outage with a thirty-second wait.
    Backoff backoff(100, 10000, 2);
    backoff.nextDelayMs();
    backoff.nextDelayMs();
    backoff.nextDelayMs();

    backoff.reset();

    TEST_ASSERT_EQUAL_UINT32(100u, backoff.nextDelayMs());
}

static void test_jitter_makes_two_devices_diverge() {
    // The thundering-herd property. Several boards rebooting after a power cut must not retry in
    // lockstep and turn their own recovery into a load spike.
    Backoff first(1000, 10000, 2);
    Backoff second(1000, 10000, 2);

    const uint32_t a = first.nextDelayMs(0);     // no jitter
    const uint32_t b = second.nextDelayMs(900);  // heavy jitter

    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_TRUE(b < a);
}

static void test_jitter_only_ever_shortens_the_wait() {
    // Retrying early is harmless; retrying late compounds with the ceiling and would make the
    // documented maximum a lie.
    for (uint16_t permille = 0; permille <= 1000; permille += 100) {
        Backoff backoff(1000, 10000, 2);
        const uint32_t delay = backoff.nextDelayMs(permille);

        TEST_ASSERT_TRUE(delay <= 1000u);
        TEST_ASSERT_TRUE(delay >= 750u);  // at most a quarter off
    }
}

static void test_backoff_counts_its_attempts() {
    Backoff backoff;
    TEST_ASSERT_EQUAL_UINT32(0u, backoff.attempts());
    backoff.nextDelayMs();
    backoff.nextDelayMs();
    TEST_ASSERT_EQUAL_UINT32(2u, backoff.attempts());
}

static void test_degenerate_settings_do_not_produce_a_zero_delay() {
    // A zero base would mean a reconnect loop with no delay at all -- a busy loop that looks like
    // a hung device and behaves like a denial of service against your own server.
    Backoff backoff(0, 0, 0);

    TEST_ASSERT_TRUE(backoff.nextDelayMs() > 0u);
}

// ---------------------------------------------------------------------------------------
// The line reader
// ---------------------------------------------------------------------------------------

static std::vector<LineReader::Line> readAll(LineReader& reader, const std::string& input) {
    std::vector<LineReader::Line> lines;
    reader.feed(input.data(), input.size(), [&](const LineReader::Line& line) {
        lines.push_back(line);
    });
    return lines;
}

static void test_a_line_ends_at_newline() {
    LineReader reader;
    const auto lines = readAll(reader, "привіт\n");

    TEST_ASSERT_EQUAL_UINT(1u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_STRING("привіт", lines[0].text.c_str());
    TEST_ASSERT_TRUE(lines[0].complete);
    TEST_ASSERT_FALSE(lines[0].truncated);
}

static void test_crlf_produces_one_line_not_two() {
    // A terminal sending CRLF must not yield an empty line after every real one.
    LineReader reader;
    const auto lines = readAll(reader, "one\r\ntwo\r\n");

    TEST_ASSERT_EQUAL_UINT(2u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_STRING("one", lines[0].text.c_str());
    TEST_ASSERT_EQUAL_STRING("two", lines[1].text.c_str());
}

static void test_blank_lines_are_ignored() {
    // Pressing return at an idle prompt should do nothing, not send an empty turn.
    LineReader reader;
    const auto lines = readAll(reader, "\n\n\nreal\n\n");

    TEST_ASSERT_EQUAL_UINT(1u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_STRING("real", lines[0].text.c_str());
}

static void test_a_partial_line_is_held_until_its_terminator() {
    LineReader reader;
    auto lines = readAll(reader, "incom");
    TEST_ASSERT_EQUAL_UINT(0u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_UINT(5u, static_cast<unsigned>(reader.pending()));

    lines = readAll(reader, "plete\n");
    TEST_ASSERT_EQUAL_UINT(1u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_STRING("incomplete", lines[0].text.c_str());
}

static void test_an_over_long_line_is_truncated_not_grown() {
    // A megabyte pasted into a terminal must not be able to exhaust 320 KB of RAM.
    LineReader reader(16);
    const auto lines = readAll(reader, std::string(500, 'x') + "\n");

    TEST_ASSERT_EQUAL_UINT(1u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_EQUAL_UINT(16u, static_cast<unsigned>(lines[0].text.size()));
    TEST_ASSERT_TRUE(lines[0].truncated);
}

static void test_the_line_after_an_overflow_starts_clean() {
    // A reader that stopped consuming at the limit would splice the tail of the over-long paste
    // onto whatever the person typed next.
    LineReader reader(8);
    const auto lines = readAll(reader, std::string(100, 'x') + "\nshort\n");

    TEST_ASSERT_EQUAL_UINT(2u, static_cast<unsigned>(lines.size()));
    TEST_ASSERT_TRUE(lines[0].truncated);
    TEST_ASSERT_EQUAL_STRING("short", lines[1].text.c_str());
    TEST_ASSERT_FALSE(lines[1].truncated);
}

static void test_clear_drops_a_partial_line() {
    LineReader reader;
    readAll(reader, "half");
    reader.clear();

    TEST_ASSERT_EQUAL_UINT(0u, static_cast<unsigned>(reader.pending()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_window_closed_by_the_server_lands_in_thinking);
    RUN_TEST(test_the_transition_table_is_total);
    RUN_TEST(test_every_state_has_a_name);
    RUN_TEST(test_the_state_names_are_the_specified_ones);
    RUN_TEST(test_the_happy_path_runs_boot_to_idle_and_back);
    RUN_TEST(test_a_fault_is_reachable_from_every_state);
    RUN_TEST(test_losing_wifi_is_reachable_from_every_state_except_offline);
    RUN_TEST(test_recovery_returns_to_idle);
    RUN_TEST(test_offline_does_not_claim_to_be_idle_before_the_socket_is_back);
    RUN_TEST(test_a_turn_that_says_nothing_still_returns_to_idle);
    RUN_TEST(test_more_deltas_while_replying_is_not_a_state_change);
    RUN_TEST(test_the_server_may_speak_first);
    RUN_TEST(test_a_turn_cannot_start_unless_idle);
    RUN_TEST(test_backoff_grows_exponentially);
    RUN_TEST(test_backoff_is_capped_at_the_ceiling);
    RUN_TEST(test_backoff_resets_after_a_success);
    RUN_TEST(test_jitter_makes_two_devices_diverge);
    RUN_TEST(test_jitter_only_ever_shortens_the_wait);
    RUN_TEST(test_backoff_counts_its_attempts);
    RUN_TEST(test_degenerate_settings_do_not_produce_a_zero_delay);
    RUN_TEST(test_a_line_ends_at_newline);
    RUN_TEST(test_crlf_produces_one_line_not_two);
    RUN_TEST(test_blank_lines_are_ignored);
    RUN_TEST(test_a_partial_line_is_held_until_its_terminator);
    RUN_TEST(test_an_over_long_line_is_truncated_not_grown);
    RUN_TEST(test_the_line_after_an_overflow_starts_clean);
    RUN_TEST(test_clear_drops_a_partial_line);
    return UNITY_END();
}

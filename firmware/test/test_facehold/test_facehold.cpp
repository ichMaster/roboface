// The ttl countdown, and the rule that decides whether the mouth moves.
//
// Both are decisions rather than drawing, so both live in `src/pure/` and are answered here rather
// than by watching a screen. The second one especially: "the mouth stopped moving" is a symptom
// that took a long evening to trace the first time, and what fixes it is one line of logic that a
// host test can hold still.

#include <unity.h>

#include "pure/facehold.h"

using roboface::FaceHold;
using roboface::appliesNow;
using roboface::mouthRuns;

// ---------------------------------------------------------------------------------------
// FaceHold
// ---------------------------------------------------------------------------------------

static void test_a_fresh_hold_is_not_held() {
    // Zero is "no server frame is in force", which is what a device-local face leaves behind:
    // `boot` and `offline` are not things that expire.
    FaceHold hold;
    TEST_ASSERT_FALSE(hold.isHeld());
    TEST_ASSERT_FALSE(hold.advance(10000));
}

static void test_a_held_frame_expires_after_its_ttl() {
    FaceHold hold;
    hold.hold(1000);

    TEST_ASSERT_TRUE(hold.isHeld());
    TEST_ASSERT_FALSE(hold.advance(400));
    TEST_ASSERT_FALSE(hold.advance(400));
    TEST_ASSERT_TRUE(hold.advance(400));
    TEST_ASSERT_FALSE(hold.isHeld());
}

static void test_expiry_is_an_edge_and_fires_once() {
    // **The property the caller depends on.** Expiry re-targets the crossfade toward neutral; a
    // predicate that stayed true would re-target it on every frame afterwards and the face would
    // never finish relaxing -- an animation permanently 150 ms from arriving.
    FaceHold hold;
    hold.hold(100);

    TEST_ASSERT_TRUE(hold.advance(100));
    for (int tick = 0; tick < 50; ++tick) {
        TEST_ASSERT_FALSE(hold.advance(100));
    }
}

static void test_a_new_frame_restarts_the_countdown() {
    // Why the ttl never expires in normal operation: the server sends a frame on every state
    // change, so each one renews the hold. What it bounds is the connection dying between two.
    FaceHold hold;
    hold.hold(1000);
    hold.advance(900);
    hold.hold(1000);

    TEST_ASSERT_FALSE(hold.advance(900));
    TEST_ASSERT_TRUE(hold.isHeld());
}

static void test_an_oversized_step_expires_rather_than_underflowing() {
    // `remaining_ms_` is unsigned, and this project has already lost hours to one unsigned
    // subtraction that wrapped to four billion. A tick longer than the ttl must expire, not turn
    // into a 49-day hold.
    FaceHold hold;
    hold.hold(50);

    TEST_ASSERT_TRUE(hold.advance(4000000000u));
    TEST_ASSERT_EQUAL_UINT32(0, hold.remaining());
}

static void test_release_drops_a_live_hold() {
    // What a dropped link does: whatever the server last said described a turn that is now
    // unreachable, and holding it would leave the device cheerful about a broken connection.
    FaceHold hold;
    hold.hold(8000);
    hold.release();

    TEST_ASSERT_FALSE(hold.isHeld());
    TEST_ASSERT_FALSE(hold.advance(1));
}

static void test_a_zero_ttl_is_simply_not_held() {
    FaceHold hold;
    hold.hold(0);
    TEST_ASSERT_FALSE(hold.isHeld());
}

// ---------------------------------------------------------------------------------------
// mouthRuns -- the v2.1.2 rule
// ---------------------------------------------------------------------------------------

static void test_the_mouth_needs_both_the_permission_and_the_fact() {
    TEST_ASSERT_TRUE(mouthRuns(true, true));
    TEST_ASSERT_FALSE(mouthRuns(true, false));
    TEST_ASSERT_FALSE(mouthRuns(false, true));
    TEST_ASSERT_FALSE(mouthRuns(false, false));
}

static void test_the_server_saying_the_reply_ended_does_not_stop_a_playing_speaker() {
    // **The `v2.1.2` regression, held still.**
    //
    // The server's `speaking: false` arrives while the device is still draining audio the server
    // finished sending -- measured at 244 KB, about eight seconds of voice. Acting on it stopped
    // the mouth mid-sentence, and the symptom ("the mouth freezes") pointed at the lip-sync, which
    // was working perfectly.
    //
    // So the case below is not hypothetical and not an edge: it is what every single reply does.
    const bool server_says_done = false;
    const bool speaker_still_playing = true;

    TEST_ASSERT_FALSE(mouthRuns(server_says_done, speaker_still_playing));
}

static void test_a_playing_speaker_with_no_permission_is_not_the_character_talking() {
    // A loopback recording or a chime plays through the same speaker. Moving the mouth for those
    // would be the device lip-syncing to something it is not saying.
    TEST_ASSERT_FALSE(mouthRuns(false, true));
}

// ---------------------------------------------------------------------------------------
// appliesNow -- the half of the rule that was missing (code review #1)
// ---------------------------------------------------------------------------------------

static void test_a_frame_that_grants_permission_always_applies_at_once() {
    // The face should change as the device *begins* to speak. That ordering is what the response
    // schema's field order and the orchestrator's yield order are both arranged around, and a
    // frame held here would undo all of it.
    TEST_ASSERT_TRUE(appliesNow(true, false));
    TEST_ASSERT_TRUE(appliesNow(true, true));
}

static void test_a_frame_with_nothing_playing_applies_at_once() {
    TEST_ASSERT_TRUE(appliesNow(false, false));
}

static void test_a_frame_that_ends_the_speaking_waits_for_the_speaker() {
    // **The bug this function exists for.** The server sends `speaking: false` when it has finished
    // the turn; the device still holds seconds of audio. Applied on arrival it shuts the mouth and
    // restarts the drift mid-sentence -- both of the faults `v2.1.2` fixed, arriving through the
    // server channel this time.
    TEST_ASSERT_FALSE(appliesNow(false, true));
}

static void test_the_whole_end_of_turn_sequence_keeps_the_mouth_running() {
    // The failure walked end to end, in the order the wire actually produces it. This is the test
    // that would have caught code review #1: `mouthRuns` was right the whole time and was being
    // asked about a permission that had already been withdrawn.
    bool permission = false;
    bool playing = false;

    // 1. The reply's frame arrives: `speaking: true`, before the first word.
    TEST_ASSERT_TRUE(appliesNow(true, playing));
    permission = true;

    // 2. The speaker starts.
    playing = true;
    TEST_ASSERT_TRUE(mouthRuns(permission, playing));

    // 3. The server finishes the turn and sends `speaking: false` -- while the device is still
    //    holding seconds of audio. Held, not applied.
    TEST_ASSERT_FALSE(appliesNow(false, playing));
    TEST_ASSERT_TRUE(mouthRuns(permission, playing));

    // 4. Several frames of playback later, the mouth is still moving.
    for (int tick = 0; tick < 100; ++tick) {
        TEST_ASSERT_TRUE(mouthRuns(permission, playing));
    }

    // 5. The speaker stops. Now the held frame applies, and the mouth shuts.
    playing = false;
    permission = false;  // what the held frame carries
    TEST_ASSERT_FALSE(mouthRuns(permission, playing));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_fresh_hold_is_not_held);
    RUN_TEST(test_a_held_frame_expires_after_its_ttl);
    RUN_TEST(test_expiry_is_an_edge_and_fires_once);
    RUN_TEST(test_a_new_frame_restarts_the_countdown);
    RUN_TEST(test_an_oversized_step_expires_rather_than_underflowing);
    RUN_TEST(test_release_drops_a_live_hold);
    RUN_TEST(test_a_zero_ttl_is_simply_not_held);
    RUN_TEST(test_the_mouth_needs_both_the_permission_and_the_fact);
    RUN_TEST(test_the_server_saying_the_reply_ended_does_not_stop_a_playing_speaker);
    RUN_TEST(test_a_playing_speaker_with_no_permission_is_not_the_character_talking);
    RUN_TEST(test_a_frame_that_grants_permission_always_applies_at_once);
    RUN_TEST(test_a_frame_with_nothing_playing_applies_at_once);
    RUN_TEST(test_a_frame_that_ends_the_speaking_waits_for_the_speaker);
    RUN_TEST(test_the_whole_end_of_turn_sequence_keeps_the_mouth_running);
    return UNITY_END();
}

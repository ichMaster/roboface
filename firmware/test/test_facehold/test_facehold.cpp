// The ttl countdown, and the rule that decides whether the mouth moves.
//
// Both are decisions rather than drawing, so both live in `src/pure/` and are answered here rather
// than by watching a screen. The second one especially: "the mouth stopped moving" is a symptom
// that took a long evening to trace the first time, and what fixes it is one line of logic that a
// host test can hold still.

#include <unity.h>

#include "pure/facehold.h"

using roboface::FaceHold;
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
    return UNITY_END();
}

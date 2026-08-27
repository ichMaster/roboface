#include <unity.h>

#include "pure/half_duplex.h"

namespace {

//: Nothing listens until something asks to.
void listening_is_off_until_it_is_wanted() {
    roboface::HalfDuplexGuard guard;
    TEST_ASSERT_FALSE(guard.shouldListen());
    guard.wantListening(true);
    TEST_ASSERT_TRUE(guard.shouldListen());
}

//: The whole point: while the device speaks, it does not hear.
void speaking_suspends_listening() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    guard.playbackStarted();
    TEST_ASSERT_FALSE(guard.shouldListen());
    TEST_ASSERT_TRUE(guard.isSpeaking());
}

//: And it hears again the moment it stops -- the normal `tts_end` route.
void listening_resumes_when_playback_ends() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    guard.playbackStarted();
    TEST_ASSERT_TRUE(guard.playbackEnded());
    TEST_ASSERT_TRUE(guard.shouldListen());
}

//: Resuming reports true exactly once, so the endpointer is cleared once rather than on every
//: loop that happens to notice.
void resuming_is_reported_once() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    guard.playbackStarted();
    TEST_ASSERT_TRUE(guard.playbackEnded());
    TEST_ASSERT_FALSE(guard.playbackEnded());
}

//: An abnormal end is still an end. This is the case that matters: a reply cancelled mid-sentence,
//: a socket dropped, a fault. Anything that leaves `speaking_` set forever leaves a device that is
//: connected, idle, and permanently deaf.
void an_abnormal_end_still_resumes_listening() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    guard.playbackStarted();
    // No `tts_end` ever arrives; the turn is aborted instead.
    TEST_ASSERT_TRUE(guard.playbackEnded());
    TEST_ASSERT_TRUE(guard.shouldListen());
}

//: Playback ending when nothing was playing is not an event, and must not claim listening resumed.
void ending_playback_that_never_started_is_not_a_resume() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    TEST_ASSERT_FALSE(guard.playbackEnded());
}

//: With listening switched off, playback has nothing to give back -- PTT-only must not be turned
//: into active listening by a reply ending.
void playback_does_not_enable_listening_that_was_never_wanted() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(false);
    guard.playbackStarted();
    TEST_ASSERT_FALSE(guard.playbackEnded());
    TEST_ASSERT_FALSE(guard.shouldListen());
}

//: Turning listening off mid-reply leaves it off afterwards.
void switching_off_during_playback_stays_off() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    guard.playbackStarted();
    guard.wantListening(false);
    TEST_ASSERT_FALSE(guard.playbackEnded());
    TEST_ASSERT_FALSE(guard.shouldListen());
}

//: Several replies in a row each suspend and resume; nothing accumulates.
void repeated_turns_each_suspend_and_resume() {
    roboface::HalfDuplexGuard guard;
    guard.wantListening(true);
    for (int turn = 0; turn < 5; ++turn) {
        guard.playbackStarted();
        TEST_ASSERT_FALSE(guard.shouldListen());
        TEST_ASSERT_TRUE(guard.playbackEnded());
        TEST_ASSERT_TRUE(guard.shouldListen());
    }
}

}  // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(listening_is_off_until_it_is_wanted);
    RUN_TEST(speaking_suspends_listening);
    RUN_TEST(listening_resumes_when_playback_ends);
    RUN_TEST(resuming_is_reported_once);
    RUN_TEST(an_abnormal_end_still_resumes_listening);
    RUN_TEST(ending_playback_that_never_started_is_not_a_resume);
    RUN_TEST(playback_does_not_enable_listening_that_was_never_wanted);
    RUN_TEST(switching_off_during_playback_stays_off);
    RUN_TEST(repeated_turns_each_suspend_and_resume);
    return UNITY_END();
}

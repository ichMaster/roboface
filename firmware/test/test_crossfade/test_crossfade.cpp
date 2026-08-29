#include <unity.h>

#include <cmath>

#include "pure/crossfade.h"
#include "pure/face.h"

namespace {

using roboface::Crossfade;
using roboface::DeviceState;
using roboface::FaceRecipe;
using roboface::recipeFor;

constexpr uint32_t kFrameMs = 20;

bool nearly(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) < tolerance; }

//: A fade lands exactly on its destination -- not near it. An easing curve that overshot or fell
//: short would leave every expression slightly wrong, forever, in a way no single frame reveals.
void a_fade_ends_exactly_on_the_target() {
    Crossfade fade{recipeFor(DeviceState::kIdle)};
    const auto target = recipeFor(DeviceState::kThinking);
    fade.target(target);
    for (uint32_t elapsed = 0; elapsed <= roboface::kCrossfadeMs + kFrameMs; elapsed += kFrameMs) {
        fade.advance(kFrameMs);
    }
    TEST_ASSERT_TRUE(nearly(fade.current().eye_openness, target.eye_openness));
    TEST_ASSERT_TRUE(nearly(fade.current().mouth_curve, target.mouth_curve));
    TEST_ASSERT_TRUE(nearly(fade.current().brow_angle, target.brow_angle));
    TEST_ASSERT_FALSE(fade.isFading());
}

//: And it starts exactly on its source: the first frame must not jump.
void a_fade_starts_from_where_the_face_is() {
    const auto idle = recipeFor(DeviceState::kIdle);
    Crossfade fade{idle};
    fade.target(recipeFor(DeviceState::kOffline));
    const auto first = fade.advance(1);
    TEST_ASSERT_TRUE(std::fabs(first.eye_openness - idle.eye_openness) < 0.05f);
}

//: Progress is monotonic. A face that moved toward an expression and briefly back would read as a
//: flicker, and easing is exactly where that kind of mistake hides.
void the_fade_is_monotonic() {
    Crossfade fade{recipeFor(DeviceState::kIdle)};
    fade.target(recipeFor(DeviceState::kListening));  // openness 0.85 -> 1.0, rising
    float previous = fade.current().eye_openness;
    for (uint32_t elapsed = 0; elapsed < roboface::kCrossfadeMs; elapsed += kFrameMs) {
        const float now = fade.advance(kFrameMs).eye_openness;
        TEST_ASSERT_TRUE(now >= previous - 0.0001f);
        previous = now;
    }
}

//: The case that happens constantly in practice and never in a demo: two state changes inside a
//: second. The second fade must start from where the face *is*, or it visibly jumps backwards
//: before moving on.
void an_interrupted_fade_does_not_jump_backwards() {
    Crossfade fade{recipeFor(DeviceState::kIdle)};
    fade.target(recipeFor(DeviceState::kOffline));       // heading somewhere dim and frowning
    for (int frame = 0; frame < 4; ++frame) fade.advance(kFrameMs);
    const auto midway = fade.current();

    fade.target(recipeFor(DeviceState::kReplying));      // interrupted, heading somewhere cheerful
    const auto next = fade.advance(1);

    // The very next frame is essentially where we were -- continuity, not a jump.
    TEST_ASSERT_TRUE(std::fabs(next.mouth_curve - midway.mouth_curve) < 0.05f);
    TEST_ASSERT_TRUE(std::fabs(next.dim - midway.dim) < 0.05f);
}

//: Two faces too similar to be worth animating settle immediately. 200 ms of nothing happening is
//: still 200 ms during which the face is not doing what it was asked to.
void a_negligible_change_settles_at_once() {
    const auto idle = recipeFor(DeviceState::kIdle);
    Crossfade fade{idle};
    FaceRecipe almost = idle;
    almost.mouth_curve += 0.01f;
    fade.target(almost);
    TEST_ASSERT_FALSE(fade.isFading());
}

//: `ttl_ms` expiry drifts home rather than snapping, and lands on rest.
void ttl_expiry_relaxes_to_the_resting_face() {
    const auto idle = recipeFor(DeviceState::kIdle);
    Crossfade fade{idle};
    fade.setResting(idle);
    fade.target(recipeFor(DeviceState::kReplying), 300);

    for (uint32_t elapsed = 0; elapsed < roboface::kCrossfadeMs + 300; elapsed += kFrameMs) {
        fade.advance(kFrameMs);
    }
    TEST_ASSERT_TRUE(fade.isRelaxing());

    for (uint32_t elapsed = 0; elapsed <= roboface::kRelaxMs + kFrameMs; elapsed += kFrameMs) {
        fade.advance(kFrameMs);
    }
    TEST_ASSERT_TRUE(nearly(fade.current().mouth_curve, idle.mouth_curve));
    TEST_ASSERT_FALSE(fade.isRelaxing());
}

//: Relaxing is gentler than deciding. If they took the same time, drifting home would look like
//: another expression being chosen.
void relaxing_is_slower_than_a_state_change() {
    TEST_ASSERT_TRUE(roboface::kRelaxMs > roboface::kCrossfadeMs * 3);
}

//: Without a lifetime an expression is held. A face that wandered home on its own would contradict
//: the server, which in v2.2 is the thing that decides how long a feeling lasts.
void an_expression_without_a_ttl_is_held() {
    Crossfade fade{recipeFor(DeviceState::kIdle)};
    const auto target = recipeFor(DeviceState::kThinking);
    fade.target(target);  // no ttl
    for (uint32_t elapsed = 0; elapsed < 30000; elapsed += kFrameMs) fade.advance(kFrameMs);
    TEST_ASSERT_TRUE(nearly(fade.current().brow_angle, target.brow_angle));
    TEST_ASSERT_FALSE(fade.isRelaxing());
}

//: The easing curve is a curve: it must not be a straight line wearing the name.
void the_easing_is_actually_eased() {
    Crossfade linear_check{FaceRecipe{0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    linear_check.target(FaceRecipe{1.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    // A quarter of the way through, an eased curve has covered noticeably less than a quarter.
    for (uint32_t elapsed = 0; elapsed < roboface::kCrossfadeMs / 4; elapsed += kFrameMs) {
        linear_check.advance(kFrameMs);
    }
    TEST_ASSERT_TRUE(linear_check.current().eye_openness < 0.20f);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(a_fade_ends_exactly_on_the_target);
    RUN_TEST(a_fade_starts_from_where_the_face_is);
    RUN_TEST(the_fade_is_monotonic);
    RUN_TEST(an_interrupted_fade_does_not_jump_backwards);
    RUN_TEST(a_negligible_change_settles_at_once);
    RUN_TEST(ttl_expiry_relaxes_to_the_resting_face);
    RUN_TEST(relaxing_is_slower_than_a_state_change);
    RUN_TEST(an_expression_without_a_ttl_is_held);
    RUN_TEST(the_easing_is_actually_eased);
    return UNITY_END();
}

#include <unity.h>

#include <cstddef>
#include <set>

#include "pure/capture_slots.h"

namespace {

//: The invariant the whole type exists for: the buffer being read is never the buffer being armed.
void the_slot_being_read_is_never_the_slot_being_armed() {
    roboface::CaptureSlots slots;
    slots.reset();
    for (int frame = 0; frame < 32; ++frame) {
        TEST_ASSERT_TRUE(slots.readSlot() != slots.spareSlot());
        slots.advance();
    }
}

//: Buffers complete in the order they were armed, so reads must cycle 0,1,2,0,1,2...
void reads_follow_the_order_the_buffers_were_armed_in() {
    roboface::CaptureSlots slots;
    slots.reset();
    const std::size_t expected[] = {0, 1, 2, 0, 1, 2, 0};
    for (std::size_t index = 0; index < 7; ++index) {
        TEST_ASSERT_EQUAL_UINT32(expected[index], slots.readSlot());
        slots.advance();
    }
}

//: The frame just consumed is the one handed back to the recorder -- nothing is left unused, and
//: nothing is armed twice.
void the_consumed_frame_becomes_the_spare() {
    roboface::CaptureSlots slots;
    slots.reset();
    for (int frame = 0; frame < 12; ++frame) {
        const std::size_t consumed = slots.readSlot();
        slots.advance();
        TEST_ASSERT_EQUAL_UINT32(consumed, slots.spareSlot());
    }
}

//: Across a full rotation every buffer is used, so none sits idle while the queue runs short.
void every_buffer_is_used_across_a_rotation() {
    roboface::CaptureSlots slots;
    slots.reset();
    std::set<std::size_t> seen;
    for (std::size_t frame = 0; frame < roboface::CaptureSlots::kCount; ++frame) {
        seen.insert(slots.readSlot());
        slots.advance();
    }
    TEST_ASSERT_EQUAL_UINT32(roboface::CaptureSlots::kCount, seen.size());
}

//: A window that ends and starts again begins from a known rotation, not wherever it stopped.
void reset_returns_to_the_starting_rotation() {
    roboface::CaptureSlots slots;
    slots.reset();
    slots.advance();
    slots.advance();
    slots.reset();
    TEST_ASSERT_EQUAL_UINT32(0, slots.readSlot());
    TEST_ASSERT_EQUAL_UINT32(2, slots.spareSlot());
}

}  // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(the_slot_being_read_is_never_the_slot_being_armed);
    RUN_TEST(reads_follow_the_order_the_buffers_were_armed_in);
    RUN_TEST(the_consumed_frame_becomes_the_spare);
    RUN_TEST(every_buffer_is_used_across_a_rotation);
    RUN_TEST(reset_returns_to_the_starting_rotation);
    return UNITY_END();
}

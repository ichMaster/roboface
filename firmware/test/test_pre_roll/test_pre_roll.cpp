#include <unity.h>

#include <vector>

#include "pure/pre_roll.h"

namespace {

//: An empty ring hands back nothing, and asks for no slot to be read.
void an_empty_ring_holds_nothing() {
    roboface::PreRollRing ring{15, 15};
    TEST_ASSERT_EQUAL_UINT32(0, ring.held());
    TEST_ASSERT_EQUAL_UINT32(15, ring.readSlot(0));  // capacity() == "no such slot"
}

//: Below capacity the ring simply fills, and reads back in the order written.
void a_partly_filled_ring_reads_oldest_first() {
    roboface::PreRollRing ring{15, 15};
    for (std::size_t i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_UINT32(i, ring.writeSlot());
    TEST_ASSERT_EQUAL_UINT32(4, ring.held());
    for (std::size_t i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_UINT32(i, ring.readSlot(i));
}

//: Past capacity the oldest frame is discarded -- pre-roll wants the last N, never the first N.
void a_full_ring_keeps_the_most_recent_frames() {
    roboface::PreRollRing ring{4, 4};
    for (std::size_t i = 0; i < 10; ++i) ring.writeSlot();
    TEST_ASSERT_EQUAL_UINT32(4, ring.held());
    // Ten writes: slots cycle 0,1,2,3,0,1,2,3,0,1 — the last four written were slots 2,3,0,1.
    TEST_ASSERT_EQUAL_UINT32(2, ring.readSlot(0));
    TEST_ASSERT_EQUAL_UINT32(3, ring.readSlot(1));
    TEST_ASSERT_EQUAL_UINT32(0, ring.readSlot(2));
    TEST_ASSERT_EQUAL_UINT32(1, ring.readSlot(3));
}

//: Reading back is contiguous and in order across the wrap -- the failure this file exists to
//: prevent is audio replayed in the wrong order, which sounds nearly right and recognises as
//: nothing.
void reads_stay_in_order_across_the_wrap() {
    roboface::PreRollRing ring{5, 5};
    for (std::size_t i = 0; i < 7; ++i) ring.writeSlot();
    std::vector<std::size_t> seen;
    for (std::size_t i = 0; i < ring.held(); ++i) seen.push_back(ring.readSlot(i));
    const std::vector<std::size_t> expected{2, 3, 4, 0, 1};
    TEST_ASSERT_EQUAL_UINT32(expected.size(), seen.size());
    for (std::size_t i = 0; i < expected.size(); ++i) TEST_ASSERT_EQUAL_UINT32(expected[i], seen[i]);
}

//: `wanted` smaller than capacity caps what is kept, so a shorter pre-roll setting does not need
//: the storage to change.
void wanted_caps_what_is_kept() {
    roboface::PreRollRing ring{15, 3};
    for (std::size_t i = 0; i < 10; ++i) ring.writeSlot();
    TEST_ASSERT_EQUAL_UINT32(3, ring.held());
}

//: Turning pre-roll off keeps nothing and refuses a write slot, rather than quietly writing
//: somewhere.
void a_disabled_ring_refuses_a_slot() {
    roboface::PreRollRing ring{15, 0};
    TEST_ASSERT_EQUAL_UINT32(15, ring.writeSlot());
    TEST_ASSERT_EQUAL_UINT32(0, ring.held());
}

//: Shrinking the setting drops what no longer fits instead of reporting frames it cannot produce.
void shrinking_wanted_drops_the_excess() {
    roboface::PreRollRing ring{15, 10};
    for (std::size_t i = 0; i < 10; ++i) ring.writeSlot();
    TEST_ASSERT_EQUAL_UINT32(10, ring.held());
    ring.setWanted(4);
    TEST_ASSERT_EQUAL_UINT32(4, ring.held());
}

//: `wanted` above capacity is clamped, not trusted -- the storage is fixed at compile time.
void wanted_cannot_exceed_capacity() {
    roboface::PreRollRing ring{5, 99};
    TEST_ASSERT_EQUAL_UINT32(5, ring.wanted());
}

//: Clearing empties it: after a window closes, the frames that led into it are part of that
//: utterance and must not be replayed into the next one.
void clear_empties_the_ring() {
    roboface::PreRollRing ring{5, 5};
    for (std::size_t i = 0; i < 5; ++i) ring.writeSlot();
    ring.clear();
    TEST_ASSERT_EQUAL_UINT32(0, ring.held());
    TEST_ASSERT_EQUAL_UINT32(0, ring.writeSlot());
}

}  // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(an_empty_ring_holds_nothing);
    RUN_TEST(a_partly_filled_ring_reads_oldest_first);
    RUN_TEST(a_full_ring_keeps_the_most_recent_frames);
    RUN_TEST(reads_stay_in_order_across_the_wrap);
    RUN_TEST(wanted_caps_what_is_kept);
    RUN_TEST(a_disabled_ring_refuses_a_slot);
    RUN_TEST(shrinking_wanted_drops_the_excess);
    RUN_TEST(wanted_cannot_exceed_capacity);
    RUN_TEST(clear_empties_the_ring);
    return UNITY_END();
}

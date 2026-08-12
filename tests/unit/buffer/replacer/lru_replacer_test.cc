#include "engine/lru_replacer.h"

#include <gtest/gtest.h>

namespace engine {
namespace {

TEST(LRUReplacerTest, EmptyReplacerHasNoVictim) {
    LRUReplacer r(4);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacerTest, UnpinMakesFrameEvictable) {
    LRUReplacer r(4);
    r.Unpin(2);
    EXPECT_EQ(r.Size(), 1u);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 2);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacerTest, PinRemovesFrameFromEvictableSet) {
    LRUReplacer r(4);
    r.Unpin(1);
    r.Pin(1);
    EXPECT_EQ(r.Size(), 0u);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
}

TEST(LRUReplacerTest, PinOnNonEvictableFrameIsSafeNoOp) {
    LRUReplacer r(4);
    r.Pin(0);
    EXPECT_EQ(r.Size(), 0u);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
}

TEST(LRUReplacerTest, UnpinTwiceIsIdempotent) {
    LRUReplacer r(4);
    r.Unpin(0);
    r.Unpin(0);
    EXPECT_EQ(r.Size(), 1u);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
}

TEST(LRUReplacerTest, VictimReturnsLeastRecentlyUnpinnedFrame) {
    LRUReplacer r(4);
    r.Unpin(0);
    r.Unpin(1);
    r.Unpin(2);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 1);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 2);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacerTest, UnpinAddsFrameToMostRecentlyUsedEnd) {
    LRUReplacer r(4);
    r.Unpin(0);
    r.Unpin(1);
    // 0 is LRU, 1 is MRU.
    // Unpinning 2 should put it after 1.
    r.Unpin(2);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 1);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 2);
}

TEST(LRUReplacerTest, PinRemovesFrameFromMiddleOfLRUOrder) {
    LRUReplacer r(4);
    r.Unpin(0);
    r.Unpin(1);
    r.Unpin(2);
    // Remove 1 from:
    // 0 -> 1 -> 2
    r.Pin(1);
    EXPECT_EQ(r.Size(), 2u);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 2);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacerTest, RepinAfterVictimSelectionThenUnpinAgainWorks) {
    LRUReplacer r(2);
    r.Unpin(0);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
    r.Pin(0);
    r.Unpin(0);
    EXPECT_EQ(r.Size(), 1u);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
}

TEST(LRUReplacerTest, ReUnpinAfterPinMakesFrameMostRecent) {
    LRUReplacer r(4);
    r.Unpin(0);
    r.Unpin(1);
    // Current order:
    // 0 -> 1
    r.Pin(0);
    r.Unpin(0);
    // New order:
    // 1 -> 0
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 1);
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(LRUReplacerTest, ManyFramesAreEvictedExactlyOnce) {
    constexpr size_t kN = 10;
    LRUReplacer r(kN);
    for (frame_id_t i = 0; i < static_cast<frame_id_t>(kN); ++i) {
        r.Unpin(i);
    }
    EXPECT_EQ(r.Size(), kN);

    std::vector<bool> seen(kN, false);
    for (size_t i = 0; i < kN; ++i) {
        frame_id_t f;
        ASSERT_TRUE(r.Victim(&f)) << "expected a victim on round " << i;
        ASSERT_FALSE(seen[static_cast<size_t>(f)]) << "frame " << f << " evicted twice";
        seen[static_cast<size_t>(f)] = true;
    }
    EXPECT_EQ(r.Size(), 0u);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
}

} // namespace
} // namespace engine
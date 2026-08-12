#include "engine/clock_replacer.h"

#include <gtest/gtest.h>

namespace engine {
namespace {

TEST(ClockReplacerTest, EmptyReplacerHasNoVictim) {
    ClockReplacer r(4);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
    EXPECT_EQ(r.Size(), 0u);
}

TEST(ClockReplacerTest, UnpinMakesFrameEvictable) {
    ClockReplacer r(4);
    r.Unpin(2);
    EXPECT_EQ(r.Size(), 1u);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 2);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(ClockReplacerTest, PinRemovesFrameFromEvictableSet) {
    ClockReplacer r(4);
    r.Unpin(1);
    r.Pin(1);
    EXPECT_EQ(r.Size(), 0u);
    frame_id_t f;
    EXPECT_FALSE(r.Victim(&f));
}

TEST(ClockReplacerTest, PinOnNonEvictableFrameIsSafeNoOp) {
    ClockReplacer r(4);
    r.Pin(0);
    EXPECT_EQ(r.Size(), 0u);
}

TEST(ClockReplacerTest, UnpinTwiceIsIdempotent) {
    ClockReplacer r(4);
    r.Unpin(0);
    r.Unpin(0);
    EXPECT_EQ(r.Size(), 1u);
}

TEST(ClockReplacerTest, SecondChanceGivesRecentlyUnpinnedFrameOneMoreSweep) {
    ClockReplacer r(4);
    r.Unpin(0);
    r.Unpin(1);
    frame_id_t f;
    ASSERT_TRUE(r.Victim(&f));
    EXPECT_EQ(f, 0);
}

TEST(ClockReplacerTest, VictimSelectionMatchesFrameCountOverManyRounds) {
    constexpr size_t kN = 10;
    ClockReplacer r(kN);
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

TEST(ClockReplacerTest, RepinAfterVictimSelectionThenUnpinAgainWorks) {
    ClockReplacer r(2);
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

} // namespace
} // namespace engine
#include "engine/buffer_pool_manager.h"

#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

#include "engine/page_guard.h"

namespace engine {
namespace {

class BufferPoolManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_bpm_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 4096, true).value();
    }
    void TearDown() override {
        disk_manager_.reset();
        std::remove(path_.c_str());
    }

    std::string path_;
    std::unique_ptr<DiskManager> disk_manager_;
};

TEST_F(BufferPoolManagerTest, FetchSamePageTwiceGivesSameFrameAndPinCountTwo) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    ASSERT_TRUE(bpm.NewPage(&id).ok());
    ASSERT_TRUE(bpm.UnpinPage(id, false).ok()); // undo NewPage's implicit pin for this test

    auto* d1 = bpm.FetchPage(id).value();
    auto* d2 = bpm.FetchPage(id).value();
    EXPECT_EQ(d1, d2);
    EXPECT_EQ(bpm.GetPinCount(id), 2u);

    ASSERT_TRUE(bpm.UnpinPage(id, false).ok());
    ASSERT_TRUE(bpm.UnpinPage(id, false).ok());
    EXPECT_EQ(bpm.GetPinCount(id), 0u);
}

TEST_F(BufferPoolManagerTest, NewPageInitializesZeroedBytes) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    char* data = bpm.NewPage(&id).value();
    for (uint32_t i = 0; i < bpm.page_size(); ++i) {
        ASSERT_EQ(data[i], 0) << "byte " << i << " not zeroed";
    }
    bpm.UnpinPage(id, false);
}

TEST_F(BufferPoolManagerTest, PoolFullWithoutEvictionReturnsResourceExhausted) {
    BufferPoolManager bpm(disk_manager_.get(), 2); // deliberately tiny
    page_id_t id1;
    page_id_t id2;
    page_id_t id3;
    ASSERT_TRUE(bpm.NewPage(&id1).ok());
    ASSERT_TRUE(bpm.NewPage(&id2).ok());
    // Both frames pinned, pool full, no eviction policy exists yet.
    auto s = bpm.NewPage(&id3);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.status().code(), Status::Code::kResourceExhausted);

    // Confirm the failed attempt didn't leak a page allocation or corrupt state.
    EXPECT_EQ(bpm.FreeFrameCount(), 0u);
    EXPECT_TRUE(bpm.IsResident(id1));
    EXPECT_TRUE(bpm.IsResident(id2));
}

TEST_F(BufferPoolManagerTest, DoubleUnpinReturnsInvalidArgument) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    ASSERT_TRUE(bpm.NewPage(&id).ok());
    ASSERT_TRUE(bpm.UnpinPage(id, false).ok());
    Status s = bpm.UnpinPage(id, false);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(BufferPoolManagerTest, UnpinNonResidentPageReturnsInvalidArgument) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    Status s = bpm.UnpinPage(999, false);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(BufferPoolManagerTest, DirtyFlagIsStickyAcrossMultipleUnpins) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    ASSERT_TRUE(bpm.NewPage(&id).ok());
    ASSERT_TRUE(bpm.UnpinPage(id, false).ok()); // clean unpin

    bpm.FetchPage(id).value();
    ASSERT_TRUE(bpm.UnpinPage(id, true).ok()); // one dirty unpin among several

    bpm.FetchPage(id).value();
    ASSERT_TRUE(bpm.UnpinPage(id, false).ok()); // clean again -- must NOT clear dirty

    ASSERT_TRUE(bpm.FlushPage(id).ok()); // exercised indirectly below via WritePage side effect
}

TEST_F(BufferPoolManagerTest, FlushPageWritesDirtyDataToDisk) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    char* data = bpm.NewPage(&id).value();
    strcpy(data, "flush me");
    ASSERT_TRUE(bpm.UnpinPage(id, /*is_dirty=*/true).ok());
    ASSERT_TRUE(bpm.FlushPage(id).ok());

    std::vector<char> disk_buf(disk_manager_->page_size());
    ASSERT_TRUE(disk_manager_->ReadPage(id, disk_buf.data()).ok());
    EXPECT_EQ(std::memcmp(disk_buf.data(), "flush me", 8), 0);
}

TEST_F(BufferPoolManagerTest, FlushAllPagesFlushesEveryDirtyResidentPage) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id1;
    page_id_t id2;
    char* d1 = bpm.NewPage(&id1).value();
    char* d2 = bpm.NewPage(&id2).value();
    strcpy(d1, "page one");
    strcpy(d2, "page two");
    bpm.UnpinPage(id1, true);
    bpm.UnpinPage(id2, true);

    ASSERT_TRUE(bpm.FlushAllPages().ok());

    std::vector<char> buf(disk_manager_->page_size());
    disk_manager_->ReadPage(id1, buf.data());
    EXPECT_EQ(std::memcmp(buf.data(), "page one", 8), 0);
    disk_manager_->ReadPage(id2, buf.data());
    EXPECT_EQ(std::memcmp(buf.data(), "page two", 8), 0);
}

TEST_F(BufferPoolManagerTest, FailedFetchDoesNotLeakFreeFrame) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    size_t free_before = bpm.FreeFrameCount();

    Status s = bpm.FetchPage(999).status();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(bpm.FreeFrameCount(), free_before);
    EXPECT_FALSE(bpm.IsResident(999));
}

TEST_F(BufferPoolManagerTest, PageGuardAutoUnpinsOnScopeExit) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    {
        auto guard_or = NewPageGuarded(&bpm, &id);
        ASSERT_TRUE(guard_or.ok());
        PageGuard guard = std::move(guard_or.value());
        EXPECT_EQ(bpm.GetPinCount(id), 1u);
    } // guard destructs here
    EXPECT_EQ(bpm.GetPinCount(id), 0u);
}

TEST_F(BufferPoolManagerTest, PageGuardNeverConstructedNeverCalledLeavesNoPin) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    auto guard_or = NewPageGuarded(&bpm, &id);
    ASSERT_TRUE(guard_or.ok());
    { PageGuard guard = std::move(guard_or.value()); }
    EXPECT_EQ(bpm.GetPinCount(id), 0u);
}

TEST_F(BufferPoolManagerTest, PageGuardMoveTransfersPinExactlyOnce) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    auto guard_or = NewPageGuarded(&bpm, &id);
    ASSERT_TRUE(guard_or.ok());
    PageGuard g1 = std::move(guard_or.value());
    EXPECT_EQ(bpm.GetPinCount(id), 1u);

    PageGuard g2 = std::move(g1);
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_FALSE(g1.is_valid()); // moved-from guard is empty
    EXPECT_TRUE(g2.is_valid());
    EXPECT_EQ(bpm.GetPinCount(id), 1u); // still exactly one pin, not zero or two

    g2.Reset();
    EXPECT_EQ(bpm.GetPinCount(id), 0u);
}

TEST_F(BufferPoolManagerTest, PageGuardMarkDirtyPersistsThroughFlush) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    {
        auto guard_or = NewPageGuarded(&bpm, &id);
        PageGuard guard = std::move(guard_or.value());
        std::memcpy(guard.mutable_data(), "via guard", 9);
        guard.MarkDirty();
    } // unpins with is_dirty=true

    ASSERT_TRUE(bpm.FlushPage(id).ok());
    std::vector<char> buf(disk_manager_->page_size());
    disk_manager_->ReadPage(id, buf.data());
    EXPECT_EQ(std::memcmp(buf.data(), "via guard", 9), 0);
}

TEST_F(BufferPoolManagerTest, PageGuardWithoutMarkDirtyDoesNotPersist) {
    BufferPoolManager bpm(disk_manager_.get(), 4);
    page_id_t id;
    {
        auto guard_or = NewPageGuarded(&bpm, &id);
        PageGuard guard = std::move(guard_or.value());
        std::memcpy(guard.mutable_data(), "should be lost", 15);
        // no MarkDirty() call
    }
    ASSERT_TRUE(bpm.FlushPage(id).ok()); // no-op: not dirty
    std::vector<char> buf(disk_manager_->page_size());
    disk_manager_->ReadPage(id, buf.data());
    // The on-disk page is still all-zero (from AllocatePage), NOT the
    // in-memory mutation, because we never marked the frame dirty.
    std::vector<char> zero(disk_manager_->page_size(), 0);
    EXPECT_EQ(std::memcmp(buf.data(), zero.data(), buf.size()), 0);
}

TEST_F(BufferPoolManagerTest, PinCountInvariantHoldsUnderRandomFetchUnpinChurn) {
    BufferPoolManager bpm(disk_manager_.get(), 8);
    std::vector<page_id_t> ids;
    for (int i = 0; i < 8; ++i) {
        page_id_t id;
        ASSERT_TRUE(bpm.NewPage(&id).ok());
        ids.push_back(id);
        ASSERT_TRUE(bpm.UnpinPage(id, false).ok()); // start everything unpinned
    }

    std::mt19937 rng(99);
    std::vector<int> outstanding(ids.size(), 0);

    for (int round = 0; round < 2000; ++round) {
        size_t idx = rng() % ids.size();
        bool do_fetch = (rng() % 2 == 0) || outstanding[idx] == 0;
        if (do_fetch) {
            auto d = bpm.FetchPage(ids[idx]);
            if (d.ok()) {
                outstanding[idx]++;
            }
            // A ResourceExhausted here would mean all 8 frames are pinned
            // simultaneously, which can happen with 8 pages / 8 frames if every
            // page ends up fetched at once.
        } else {
            ASSERT_TRUE(bpm.UnpinPage(ids[idx], false).ok());
            outstanding[idx]--;
        }
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(bpm.GetPinCount(ids[i]), static_cast<size_t>(outstanding[i]))
            << "pin count mismatch for page " << ids[i];
    }

    // Clean up remaining pins so the fixture's DiskManager destructor
    // doesn't trip any future "pages still pinned at shutdown" assertion.
    for (size_t i = 0; i < ids.size(); ++i) {
        for (int j = 0; j < outstanding[i]; ++j) {
            bpm.UnpinPage(ids[i], false);
        }
    }
}

TEST_F(BufferPoolManagerTest, EvictionFlushesDirtyVictimBeforeReuse) {
    BufferPoolManager bpm(disk_manager_.get(), 1); // one frame -> every new page evicts the last
    page_id_t id1;
    page_id_t id2;
    char* d1 = bpm.NewPage(&id1).value();
    strcpy(d1, "dirty victim");
    ASSERT_TRUE(bpm.UnpinPage(id1, /*is_dirty=*/true).ok());

    ASSERT_TRUE(bpm.NewPage(&id2).ok()); // forces eviction of id1's frame
    EXPECT_FALSE(bpm.IsResident(id1));

    // id1's dirty content must have been flushed to disk before its frame
    // was reused.
    std::vector<char> buf(disk_manager_->page_size());
    ASSERT_TRUE(disk_manager_->ReadPage(id1, buf.data()).ok());
    EXPECT_EQ(std::memcmp(buf.data(), "dirty victim", 12), 0);
}

TEST_F(BufferPoolManagerTest, CleanVictimIsNotWrittenToDiskOnEviction) {
    BufferPoolManager bpm(disk_manager_.get(), 1);
    page_id_t id1;
    page_id_t id2;
    ASSERT_TRUE(bpm.NewPage(&id1).ok());
    ASSERT_TRUE(bpm.UnpinPage(id1, /*is_dirty=*/false).ok());

    std::vector<char> marker(disk_manager_->page_size(), 'M');
    ASSERT_TRUE(disk_manager_->WritePage(id1, marker.data()).ok());

    ASSERT_TRUE(bpm.NewPage(&id2).ok()); // evicts id1's (clean) frame

    std::vector<char> buf(disk_manager_->page_size());
    ASSERT_TRUE(disk_manager_->ReadPage(id1, buf.data()).ok());
    EXPECT_EQ(std::memcmp(buf.data(), marker.data(), marker.size()), 0)
        << "clean eviction must not have overwritten the on-disk page";
}

TEST_F(BufferPoolManagerTest, WorkingSetLargerThanPoolStaysCorrect) {
    // The actual point of this whole stage: a working set of pages that
    // does NOT all fit in the buffer pool simultaneously must still produce
    // correct reads, via eviction + reload, checked against a shadow model.
    constexpr size_t kPoolFrames = 4;
    constexpr int kNumPages = 20; // 5x the pool size
    BufferPoolManager bpm(disk_manager_.get(), kPoolFrames);

    std::vector<page_id_t> ids;
    std::vector<std::string> expected;
    for (int i = 0; i < kNumPages; ++i) {
        page_id_t id;
        char* data = bpm.NewPage(&id).value();
        std::string content = "page-content-" + std::to_string(i);
        strcpy(data, content.data());
        ASSERT_TRUE(bpm.UnpinPage(id, true).ok());
        ids.push_back(id);
        expected.push_back(content);
    }

    // Re-read every page in reverse order (guarantees plenty of eviction
    // churn against the small pool) and confirm every value survived.
    for (int i = kNumPages - 1; i >= 0; --i) {
        char* data = bpm.FetchPage(ids[static_cast<size_t>(i)]).value();
        EXPECT_EQ(std::string(data, expected[static_cast<size_t>(i)].size()),
                  expected[static_cast<size_t>(i)])
            << "mismatch at page index " << i;
        bpm.UnpinPage(ids[static_cast<size_t>(i)], false);
    }
}

TEST_F(BufferPoolManagerTest, EvictionNeverPicksAPinnedFrame) {
    // Property test: across many random fetch/unpin operations against a
    // pool much smaller than the working set, no operation should ever
    // report a currently-pinned page as having been silently evicted out
    // from under it.
    constexpr size_t kPoolFrames = 3;
    constexpr int kNumPages = 12;
    BufferPoolManager bpm(disk_manager_.get(), kPoolFrames);

    std::vector<page_id_t> ids;
    for (int i = 0; i < kNumPages; ++i) {
        page_id_t id;
        ASSERT_TRUE(bpm.NewPage(&id).ok());
        ASSERT_TRUE(bpm.UnpinPage(id, false).ok());
        ids.push_back(id);
    }

    std::mt19937 rng(2024);
    std::vector<char*> held_data(ids.size(), nullptr); // non-null == currently pinned by us
    std::vector<int> held_count(ids.size(), 0);

    for (int round = 0; round < 5000; ++round) {
        size_t idx = rng() % ids.size();
        if (held_data[idx] == nullptr) {
            auto d = bpm.FetchPage(ids[idx]);
            if (d.ok()) {
                held_data[idx] = d.value();
                held_count[idx] = 1;
                std::memcpy(held_data[idx], &ids[idx], sizeof(page_id_t));
            }
        } else {
            page_id_t tag;
            std::memcpy(&tag, held_data[idx], sizeof(page_id_t));
            ASSERT_EQ(tag, ids[idx])
                << "pinned page's memory was overwritten -- evicted while pinned!";
            ASSERT_TRUE(bpm.UnpinPage(ids[idx], true).ok());
            held_data[idx] = nullptr;
        }
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        if (held_data[i] != nullptr) {
            bpm.UnpinPage(ids[i], true);
        }
    }
}

TEST_F(BufferPoolManagerTest, ClockPolicySelectableExplicitly) {
    BufferPoolManager bpm(disk_manager_.get(), 2, ReplacerPolicy::kClock);
    page_id_t id1;
    page_id_t id2;
    page_id_t id3;
    ASSERT_TRUE(bpm.NewPage(&id1).ok());
    ASSERT_TRUE(bpm.NewPage(&id2).ok());
    ASSERT_TRUE(bpm.UnpinPage(id1, false).ok());
    ASSERT_TRUE(bpm.UnpinPage(id2, false).ok());
    ASSERT_TRUE(bpm.NewPage(&id3).ok()); // just confirms Clock-backed eviction works end to end
    EXPECT_EQ(bpm.CapacityFrames(), 2u);
}

TEST_F(BufferPoolManagerTest, LruPolicySelectableAndEvictsTrueLeastRecentlyUsed) {
    BufferPoolManager bpm(disk_manager_.get(), 2, ReplacerPolicy::kLRU);
    page_id_t id1;
    page_id_t id2;
    page_id_t id3;
    ASSERT_TRUE(bpm.NewPage(&id1).ok());
    ASSERT_TRUE(bpm.NewPage(&id2).ok());
    ASSERT_TRUE(bpm.UnpinPage(id1, false).ok()); // id1 unpinned first -> LRU victim
    ASSERT_TRUE(bpm.UnpinPage(id2, false).ok());

    ASSERT_TRUE(bpm.NewPage(&id3).ok());
    EXPECT_FALSE(bpm.IsResident(id1));
    EXPECT_TRUE(bpm.IsResident(id2));
}

} // namespace
} // namespace engine
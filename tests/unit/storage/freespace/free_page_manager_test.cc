#include "engine/free_page_manager.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <random>
#include <set>

#include "engine/free_list_node.h"

namespace engine {
namespace {

class FreePageManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_fpm_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 4096, true).value();
        bpm_ = std::make_unique<BufferPoolManager>(disk_manager_.get(), 16);
        fpm_ = std::make_unique<FreePageManager>(disk_manager_.get(), bpm_.get());
    }
    void TearDown() override {
        fpm_.reset();
        bpm_.reset();
        disk_manager_.reset();
        std::remove(path_.c_str());
    }

    std::string path_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<FreePageManager> fpm_;
};

TEST_F(FreePageManagerTest, FreshDatabaseHasEmptyFreeList) {
    EXPECT_EQ(fpm_->FreeListHead(), kInvalidPageId);
    auto walk = fpm_->DebugWalkFreeList();
    ASSERT_TRUE(walk.ok());
    EXPECT_TRUE(walk.value().empty());
}

TEST_F(FreePageManagerTest, AllocateWithEmptyFreeListExtendsFile) {
    uint32_t pages_before = disk_manager_->GetNumPages();
    page_id_t id;
    auto data_or = fpm_->AllocateAndPinPage(&id);
    ASSERT_TRUE(data_or.ok());
    EXPECT_EQ(disk_manager_->GetNumPages(), pages_before + 1);
    bpm_->UnpinPage(id, false);
}

TEST_F(FreePageManagerTest, AllocateDeallocateAllocateReusesTheId) {
    page_id_t id1;
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id1).ok());
    ASSERT_TRUE(bpm_->UnpinPage(id1, false).ok());

    uint32_t pages_before = disk_manager_->GetNumPages();
    ASSERT_TRUE(fpm_->DeallocatePage(id1).ok());
    EXPECT_EQ(fpm_->FreeListHead(), id1);

    page_id_t id2;
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id2).ok());
    EXPECT_EQ(id2, id1) << "expected the freed page to be reused, not a new one allocated";
    EXPECT_EQ(disk_manager_->GetNumPages(), pages_before) << "file must not have grown";
    bpm_->UnpinPage(id2, false);
}

TEST_F(FreePageManagerTest, DeallocatingAPinnedPageIsRejected) {
    page_id_t id;
    auto data_or = fpm_->AllocateAndPinPage(&id);
    ASSERT_TRUE(data_or.ok());
    Status s = fpm_->DeallocatePage(id);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    bpm_->UnpinPage(id, false); // clean up
}

TEST_F(FreePageManagerTest, ImmediateDoubleFreeIsRejected) {
    page_id_t id;
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id).ok());
    ASSERT_TRUE(bpm_->UnpinPage(id, false).ok());
    ASSERT_TRUE(fpm_->DeallocatePage(id).ok());

    Status s = fpm_->DeallocatePage(id); // same page, no intervening allocate
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(FreePageManagerTest, ChainOfDeallocationsWalksInLifoOrder) {
    page_id_t id1;
    page_id_t id2;
    page_id_t id3;
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id1).ok());
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id2).ok());
    ASSERT_TRUE(fpm_->AllocateAndPinPage(&id3).ok());
    bpm_->UnpinPage(id1, false);
    bpm_->UnpinPage(id2, false);
    bpm_->UnpinPage(id3, false);

    ASSERT_TRUE(fpm_->DeallocatePage(id1).ok());
    ASSERT_TRUE(fpm_->DeallocatePage(id2).ok());
    ASSERT_TRUE(fpm_->DeallocatePage(id3).ok());

    // Free list is a stack: most-recently-freed is popped first.
    auto walk_or = fpm_->DebugWalkFreeList();
    ASSERT_TRUE(walk_or.ok());
    std::vector<page_id_t> expected = {id3, id2, id1};
    EXPECT_EQ(walk_or.value(), expected);
}

TEST_F(FreePageManagerTest, FreeListSurvivesReopen) {
    page_id_t id1;
    page_id_t id2;
    {
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id1).ok());
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id2).ok());
        bpm_->UnpinPage(id1, false);
        bpm_->UnpinPage(id2, false);
        ASSERT_TRUE(fpm_->DeallocatePage(id1).ok());
        ASSERT_TRUE(fpm_->DeallocatePage(id2).ok());
        ASSERT_TRUE(bpm_->FlushAllPages().ok());
        ASSERT_TRUE(disk_manager_->Shutdown().ok());
    }
    {
        auto dm2_or = DiskManager::Open(path_, 4096, false);
        ASSERT_TRUE(dm2_or.ok()) << dm2_or.status().ToString();
        auto dm2 = std::move(dm2_or.value());
        BufferPoolManager bpm2(dm2.get(), 16);
        FreePageManager fpm2(dm2.get(), &bpm2);

        EXPECT_EQ(fpm2.FreeListHead(), id2);
        auto walk_or = fpm2.DebugWalkFreeList();
        ASSERT_TRUE(walk_or.ok());
        std::vector<page_id_t> expected = {id2, id1};
        EXPECT_EQ(walk_or.value(), expected);
    }
}

TEST_F(FreePageManagerTest, FileSizePlateausUnderSteadyChurn) {
    std::vector<page_id_t> ids;
    for (int i = 0; i < 10; ++i) {
        page_id_t id;
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id).ok());
        bpm_->UnpinPage(id, false);
        ids.push_back(id);
    }
    for (page_id_t id : ids) {
        ASSERT_TRUE(fpm_->DeallocatePage(id).ok());
    }

    uint32_t steady_state_pages = disk_manager_->GetNumPages();

    std::mt19937 rng(11);
    for (int round = 0; round < 500; ++round) {
        page_id_t id;
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id).ok());
        bpm_->UnpinPage(id, false);
        ASSERT_TRUE(fpm_->DeallocatePage(id).ok());
    }
    EXPECT_EQ(disk_manager_->GetNumPages(), steady_state_pages)
        << "file grew during alloc/free churn of an already-freed working set";
}

TEST_F(FreePageManagerTest, PropertyRandomAllocDeallocMatchesSetOracle) {
    std::set<page_id_t> allocated;
    std::vector<page_id_t> ever_allocated;
    std::mt19937 rng(2025);

    for (int round = 0; round < 300; ++round) {
        bool do_alloc = allocated.empty() || (rng() % 2 == 0);
        if (do_alloc) {
            page_id_t id;
            auto data_or = fpm_->AllocateAndPinPage(&id);
            ASSERT_TRUE(data_or.ok());
            bpm_->UnpinPage(id, false);
            allocated.insert(id);
            ever_allocated.push_back(id);
        } else {
            auto it = allocated.begin();
            std::advance(it, rng() % allocated.size());
            page_id_t id = *it;
            ASSERT_TRUE(fpm_->DeallocatePage(id).ok());
            allocated.erase(it);
        }
    }

    auto free_walk_or = fpm_->DebugWalkFreeList();
    ASSERT_TRUE(free_walk_or.ok());
    std::set<page_id_t> on_free_list(free_walk_or.value().begin(), free_walk_or.value().end());

    for (page_id_t id : ever_allocated) {
        bool is_allocated = allocated.contains(id);
        bool is_free = on_free_list.contains(id);
        EXPECT_TRUE(is_allocated != is_free)
            << "page " << id << " allocated=" << is_allocated << " free=" << is_free;
    }
}

TEST_F(FreePageManagerTest, CorruptedChainLinkIsDetectedAsCorruption) {
    page_id_t id1;
    page_id_t id2;
    {
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id1).ok());
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id2).ok());
        bpm_->UnpinPage(id1, false);
        bpm_->UnpinPage(id2, false);
        ASSERT_TRUE(fpm_->DeallocatePage(id1).ok());
        ASSERT_TRUE(bpm_->FlushAllPages().ok());
        ASSERT_TRUE(disk_manager_->Shutdown().ok());
    }
    auto dm2 = DiskManager::Open(path_, 4096, false).value();
    std::vector<char> buf(dm2->page_size());
    ASSERT_TRUE(dm2->ReadPage(id1, buf.data()).ok());
    buf[0] = 0x7F; // not a valid PageType
    ASSERT_TRUE(dm2->WritePage(id1, buf.data()).ok());
    ASSERT_TRUE(dm2->Shutdown().ok());

    auto dm3 = DiskManager::Open(path_, 4096, false).value();
    BufferPoolManager bpm3(dm3.get(), 16);
    FreePageManager fpm3(dm3.get(), &bpm3);

    page_id_t out;
    Status s = fpm3.AllocateAndPinPage(&out).status();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kCorruption);
}

TEST_F(FreePageManagerTest, CycleInFreeListIsDetectedNotInfiniteLooped) {
    page_id_t id1;
    page_id_t id2;
    {
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id1).ok());
        ASSERT_TRUE(fpm_->AllocateAndPinPage(&id2).ok());
        bpm_->UnpinPage(id1, false);
        bpm_->UnpinPage(id2, false);
        ASSERT_TRUE(fpm_->DeallocatePage(id1).ok());
        ASSERT_TRUE(fpm_->DeallocatePage(id2).ok());
        // Chain is now: head=id2 -> id1 -> kInvalidPageId
        ASSERT_TRUE(bpm_->FlushAllPages().ok());
        ASSERT_TRUE(disk_manager_->Shutdown().ok());
    }
    auto dm2 = DiskManager::Open(path_, 4096, false).value();
    std::vector<char> buf(dm2->page_size());
    // Manually rewrite id1's next-pointer to point back at id2, creating a
    // cycle: id2 -> id1 -> id2 -> id1 -> ... forever, if not detected.
    FreeListNode::SerializeTo(buf.data(), dm2->page_size(), id2);
    ASSERT_TRUE(dm2->WritePage(id1, buf.data()).ok());
    ASSERT_TRUE(dm2->Shutdown().ok());

    auto dm3 = DiskManager::Open(path_, 4096, false).value();
    BufferPoolManager bpm3(dm3.get(), 16);
    FreePageManager fpm3(dm3.get(), &bpm3);

    auto walk_or = fpm3.DebugWalkFreeList(/*max_length=*/1000);
    EXPECT_FALSE(walk_or.ok());
    EXPECT_EQ(walk_or.status().code(), Status::Code::kCorruption);
}

} // namespace
} // namespace engine
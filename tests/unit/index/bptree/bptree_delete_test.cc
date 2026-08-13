#include "engine/b_plus_tree.h"

#include <algorithm>
#include <cstdio>
#include <gtest/gtest.h>
#include <map>
#include <random>

namespace engine {
namespace {

class BPlusTreeDeleteTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_bptree_delete_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 256, true).value();
        bpm_ = std::make_unique<BufferPoolManager>(disk_manager_.get(), 32);
        fpm_ = std::make_unique<FreePageManager>(disk_manager_.get(), bpm_.get());
        tree_ = std::make_unique<BPlusTree>(disk_manager_.get(), bpm_.get(), fpm_.get());
    }
    void TearDown() override {
        tree_.reset();
        fpm_.reset();
        bpm_.reset();
        disk_manager_.reset();
        std::remove(path_.c_str());
    }

    std::string path_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<FreePageManager> fpm_;
    std::unique_ptr<BPlusTree> tree_;
};

TEST_F(BPlusTreeDeleteTest, RemoveFromEmptyTreeReturnsNotFound) {
    Status s = tree_->Remove(Slice("anything"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kNotFound);
}

TEST_F(BPlusTreeDeleteTest, InsertThenRemoveThenGetReturnsNotFound) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(tree_->Remove(Slice("a")).ok());
    auto v = tree_->Get(Slice("a"));
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.status().code(), Status::Code::kNotFound);
}

TEST_F(BPlusTreeDeleteTest, RemoveLastKeyEmptiesTreeBackToTrueEmpty) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    EXPECT_FALSE(tree_->IsEmpty());
    ASSERT_TRUE(tree_->Remove(Slice("a")).ok());
    EXPECT_TRUE(tree_->IsEmpty());
    EXPECT_EQ(disk_manager_->GetRootPageId(), kInvalidPageId);
}

TEST_F(BPlusTreeDeleteTest, RemoveMissingKeyOnNonEmptyTreeReturnsNotFound) {
    ASSERT_TRUE(tree_->Insert(Slice("present"), Slice("v")).ok());
    Status s = tree_->Remove(Slice("absent"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kNotFound);
    EXPECT_TRUE(tree_->Get(Slice("present")).ok());
}

TEST_F(BPlusTreeDeleteTest, RootShrinksAfterMergingDownToOneChild) {
    int count = 0;
    while (tree_->Height().value() <= 1) {
        ASSERT_TRUE(tree_->Insert(Slice("k" + std::to_string(count)), Slice("0123456789")).ok());
        ++count;
        ASSERT_LT(count, 1000);
    }
    int height_after_growth = tree_->Height().value();
    ASSERT_GT(height_after_growth, 1);

    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(tree_->Remove(Slice("k" + std::to_string(i))).ok()) << "removing k" << i;
        ASSERT_TRUE(tree_->Verify().ok()) << "Verify() failed after removing k" << i;
    }
    EXPECT_TRUE(tree_->IsEmpty());
}

std::string MakeKey(int i) {
    return "k" + std::to_string(1000000 + i).substr(1);
}

TEST_F(BPlusTreeDeleteTest, InterleavedInsertAndDeleteStaysCorrect) {
    constexpr int kN = 100;

    for (int i = 0; i < kN; ++i) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }

    for (int i = 0; i < kN; i += 2) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Remove(Slice(key)).ok());
    }

    ASSERT_TRUE(tree_->Verify().ok());

    for (int i = 0; i < kN; ++i) {
        const std::string key = MakeKey(i);
        auto v = tree_->Get(Slice(key));

        if (i % 2 == 0) {
            EXPECT_FALSE(v.ok()) << key << " should have been removed";
        } else {
            ASSERT_TRUE(v.ok()) << key << " should still be present";
            EXPECT_EQ(v.value(), "v" + std::to_string(i));
        }
    }
}

TEST_F(BPlusTreeDeleteTest, InsertAllThenDeleteAllInRandomOrderEndsCleanlyEmpty) {
    constexpr int kN = 300;
    std::vector<int> order(kN);

    for (int i = 0; i < kN; ++i) {
        order[static_cast<size_t>(i)] = i;
    }

    for (int i : order) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }

    ASSERT_TRUE(tree_->Verify().ok());
    ASSERT_GT(tree_->Height().value(), 1);

    std::mt19937 rng(55);
    std::shuffle(order.begin(), order.end(), rng);

    for (int i : order) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Remove(Slice(key)).ok()) << "removing index " << i;
        ASSERT_TRUE(tree_->Verify().ok()) << "Verify() failed after removing index " << i;
    }

    EXPECT_TRUE(tree_->IsEmpty());
    EXPECT_EQ(disk_manager_->GetRootPageId(), kInvalidPageId);
}

TEST_F(BPlusTreeDeleteTest, PropertyRandomInsertAndDeleteMatchesMapOracleWithVerifyAfterEveryOp) {
    std::map<std::string, std::string> oracle;
    std::mt19937 rng(777);

    for (int round = 0; round < 800; ++round) {
        bool do_insert = oracle.empty() || (rng() % 2 == 0);

        if (do_insert) {
            std::string key = "k" + std::to_string(rng() % 300);
            std::string value = "v" + std::to_string(round);

            Status s = tree_->Insert(Slice(key), Slice(value));
            bool already_present = oracle.contains(key);

            if (already_present) {
                ASSERT_FALSE(s.ok());
                ASSERT_EQ(s.code(), Status::Code::kInvalidArgument);
            } else {
                ASSERT_TRUE(s.ok())
                    << "round " << round << " insert " << key << ": " << s.ToString();
                oracle[key] = value;
            }
        } else {
            auto it = oracle.begin();
            std::advance(it, rng() % oracle.size());

            std::string key = it->first;
            Status s = tree_->Remove(Slice(key));

            ASSERT_TRUE(s.ok()) << "round " << round << " remove " << key << ": " << s.ToString();

            oracle.erase(it);
        }

        ASSERT_TRUE(tree_->Verify().ok())
            << "Verify() failed after round " << round << " (oracle size=" << oracle.size() << ")";
    }

    if (oracle.empty()) {
        EXPECT_TRUE(tree_->IsEmpty());
    } else {
        for (const auto& [k, v] : oracle) {
            auto got = tree_->Get(Slice(k));
            ASSERT_TRUE(got.ok()) << "key " << k;
            EXPECT_EQ(got.value(), v);
        }
    }
}

TEST_F(BPlusTreeDeleteTest, PersistsAcrossReopenAfterDeletesAndMerges) {
    constexpr int kN = 150;

    for (int i = 0; i < kN; ++i) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }

    for (int i = 0; i < kN; i += 3) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Remove(Slice(key)).ok());
    }

    ASSERT_TRUE(bpm_->FlushAllPages().ok());
    ASSERT_TRUE(disk_manager_->Shutdown().ok());

    auto dm2 = DiskManager::Open(path_, 256, false).value();
    BufferPoolManager bpm2(dm2.get(), 32);
    FreePageManager fpm2(dm2.get(), &bpm2);
    BPlusTree tree2(dm2.get(), &bpm2, &fpm2);

    ASSERT_TRUE(tree2.Verify().ok());

    for (int i = 0; i < kN; ++i) {
        const std::string key = MakeKey(i);
        auto v = tree2.Get(Slice(key));

        if (i % 3 == 0) {
            EXPECT_FALSE(v.ok()) << key << " should have been removed";
        } else {
            ASSERT_TRUE(v.ok()) << key;
            EXPECT_EQ(v.value(), "v" + std::to_string(i));
        }
    }
}

TEST_F(BPlusTreeDeleteTest, FileSizeStopsGrowingUnderSteadyInsertDeleteChurn) {
    constexpr int kWorkingSetSize = 60;

    for (int i = 0; i < kWorkingSetSize; ++i) {
        const std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("0123456789")).ok());
    }

    uint32_t pages_after_initial_fill = disk_manager_->GetNumPages();

    std::mt19937 rng(9);

    for (int round = 0; round < 400; ++round) {
        int idx = static_cast<int>(rng() % kWorkingSetSize);
        const std::string key = MakeKey(idx);

        Status rs = tree_->Remove(Slice(key));

        if (rs.ok()) {
            ASSERT_TRUE(tree_->Insert(Slice(key), Slice("0123456789")).ok());
        }
    }

    ASSERT_TRUE(tree_->Verify().ok());
    EXPECT_LT(disk_manager_->GetNumPages(), pages_after_initial_fill * 3);
}

} // namespace
} // namespace engine
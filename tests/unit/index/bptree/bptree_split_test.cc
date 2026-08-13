#include <cstdio>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <random>

#include "engine/b_plus_tree.h"

namespace engine {
namespace {

class BPlusTreeSplitTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_bptree_split_" + std::to_string(::getpid()) + ".db";
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

TEST_F(BPlusTreeSplitTest, SequentialInsertGrowsMultipleLevels) {
    constexpr int kNumKeys = 400;

    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = std::format("k{:06}", i);
        std::string value = "v" + std::to_string(i);

        ASSERT_TRUE(tree_->Insert(Slice(key), Slice(value)).ok())
            << "failed inserting key index " << i;
    }

    ASSERT_TRUE(tree_->Verify().ok());

    EXPECT_GT(tree_->Height().value(), 2)
        << "expected multi-level growth over 400 sequential inserts";

    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = std::format("k{:06}", i);

        auto v = tree_->Get(Slice(key));

        ASSERT_TRUE(v.ok()) << "lost key index " << i;
        EXPECT_EQ(v.value(), "v" + std::to_string(i));
    }
}

TEST_F(BPlusTreeSplitTest, RandomInsertGrowsMultipleLevelsAndStaysBalanced) {
    constexpr int kNumKeys = 400;
    std::vector<int> order(kNumKeys);
    for (int i = 0; i < kNumKeys; ++i) {
        order[static_cast<size_t>(i)] = i;
    }

    std::mt19937 rng(123);
    std::shuffle(order.begin(), order.end(), rng);

    for (int i : order) {
        std::string key = std::format("k{:06}", i);
        std::string value = "v" + std::to_string(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice(value)).ok());
    }

    ASSERT_TRUE(tree_->Verify().ok());
    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = std::format("k{:06}", i);

        auto v = tree_->Get(Slice(key));

        ASSERT_TRUE(v.ok()) << "lost key index " << i;
        EXPECT_EQ(v.value(), "v" + std::to_string(i));
    }
}

TEST_F(BPlusTreeSplitTest, PropertyRandomInsertMatchesMapOracleWithVerifyAfterEveryOp) {
    std::map<std::string, std::string> oracle;
    std::mt19937 rng(2024);
    int successful = 0;

    for (int round = 0; round < 600; ++round) {
        std::string key = "k" + std::to_string(rng() % 500);
        std::string value = "v" + std::to_string(round);
        Status s = tree_->Insert(Slice(key), Slice(value));
        bool already_present = oracle.contains(key);
        if (already_present) {
            ASSERT_FALSE(s.ok());
            ASSERT_EQ(s.code(), Status::Code::kInvalidArgument);
        } else {
            ASSERT_TRUE(s.ok()) << "round " << round << ": " << s.ToString();
            oracle[key] = value;
            ++successful;
        }
        ASSERT_TRUE(tree_->Verify().ok()) << "Verify() failed after round " << round;
    }

    ASSERT_GT(successful, 0);
    for (const auto& [k, v] : oracle) {
        auto got = tree_->Get(Slice(k));
        ASSERT_TRUE(got.ok()) << "key " << k;
        EXPECT_EQ(got.value(), v);
    }
}

TEST_F(BPlusTreeSplitTest, PersistsAcrossReopenAfterMultipleSplits) {
    constexpr int kNumKeys = 200;

    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = std::format("k{:06}", i);
        std::string value = "v" + std::to_string(i);

        ASSERT_TRUE(tree_->Insert(Slice(key), Slice(value)).ok());
    }

    int height_before = tree_->Height().value();
    ASSERT_GT(height_before, 1);
    ASSERT_TRUE(bpm_->FlushAllPages().ok());
    ASSERT_TRUE(disk_manager_->Shutdown().ok());

    auto dm2 = DiskManager::Open(path_, 256, false).value();
    BufferPoolManager bpm2(dm2.get(), 32);
    FreePageManager fpm2(dm2.get(), &bpm2);
    BPlusTree tree2(dm2.get(), &bpm2, &fpm2);

    EXPECT_EQ(tree2.Height().value(), height_before);
    ASSERT_TRUE(tree2.Verify().ok());

    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = std::format("k{:06}", i);

        auto v = tree2.Get(Slice(key));

        ASSERT_TRUE(v.ok()) << "lost key index " << i << " after reopen";
        EXPECT_EQ(v.value(), "v" + std::to_string(i));
    }
}

TEST_F(BPlusTreeSplitTest, KeyEqualToSeparatorIsFoundInRightSubtreeAfterSplit) {
    int count = 0;
    while (tree_->Height().value() <= 1) {
        std::string key = "k" + std::to_string(count);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v")).ok());
        ++count;
        ASSERT_LT(count, 1000);
    }
    for (int i = 0; i < count; ++i) {
        std::string key = "k" + std::to_string(i);
        EXPECT_TRUE(tree_->Get(Slice(key)).ok()) << "key " << key << " not found post-split";
    }
}

} // namespace
} // namespace engine
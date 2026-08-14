#include "engine/b_plus_tree.h"

#include <algorithm>
#include <cstdio>
#include <gtest/gtest.h>
#include <map>
#include <random>

namespace engine {
namespace {

std::string MakeKey(int i) {
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "k%06d", i);
    return {buf.data()};
}

class BPlusTreeIteratorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_bptree_iter_" + std::to_string(::getpid()) + ".db";
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

TEST_F(BPlusTreeIteratorTest, BeginOnEmptyTreeIsImmediatelyInvalid) {
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    EXPECT_FALSE(it_or.value().Valid());
}

TEST_F(BPlusTreeIteratorTest, SeekOnEmptyTreeIsImmediatelyInvalid) {
    auto it_or = tree_->Begin(Slice("anything"));
    ASSERT_TRUE(it_or.ok());
    EXPECT_FALSE(it_or.value().Valid());
}

TEST_F(BPlusTreeIteratorTest, SingleKeyScanReturnsExactlyThatKey) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ToString(), "a");
    EXPECT_EQ(it.Value().ToString(), "1");
    ASSERT_TRUE(it.Next().ok());
    EXPECT_FALSE(it.Valid());
}

TEST_F(BPlusTreeIteratorTest, FullScanAcrossMultipleLeavesReturnsSortedOrder) {
    constexpr int kN = 200;
    std::vector<int> order(kN);
    for (int i = 0; i < kN; ++i) {
        order[static_cast<size_t>(i)] = i;
    }
    std::mt19937 rng(11);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i : order) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }
    ASSERT_GT(tree_->Height().value(), 1);
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    std::vector<std::string> scanned_keys;
    while (it.Valid()) {
        scanned_keys.push_back(it.Key().ToString());
        ASSERT_TRUE(it.Next().ok());
    }

    ASSERT_EQ(scanned_keys.size(), static_cast<size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        EXPECT_EQ(scanned_keys[static_cast<size_t>(i)], MakeKey(i)) << "at position " << i;
    }
    for (size_t i = 1; i < scanned_keys.size(); ++i) {
        EXPECT_LT(scanned_keys[i - 1], scanned_keys[i]);
    }
}

TEST_F(BPlusTreeIteratorTest, SeekToExistingKeyStartsExactlyThere) {
    constexpr int kN = 100;
    for (int i = 0; i < kN; ++i) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }
    std::string seek_key = MakeKey(42);
    auto it_or = tree_->Begin(Slice(seek_key));
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ToString(), seek_key);
    int expected = 42;
    while (it.Valid()) {
        EXPECT_EQ(it.Key().ToString(), MakeKey(expected));
        ASSERT_TRUE(it.Next().ok());
        ++expected;
    }
    EXPECT_EQ(expected, kN);
}

TEST_F(BPlusTreeIteratorTest, SeekToKeyBetweenExistingKeysStartsAtNextHigher) {
    ASSERT_TRUE(tree_->Insert(Slice("b"), Slice("2")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("d"), Slice("4")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("f"), Slice("6")).ok());
    auto it_or = tree_->Begin(Slice("c"));
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ToString(), "d");
}

TEST_F(BPlusTreeIteratorTest, SeekPastEveryKeyIsImmediatelyInvalid) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("b"), Slice("2")).ok());
    auto it_or = tree_->Begin(Slice("z"));
    ASSERT_TRUE(it_or.ok());
    EXPECT_FALSE(it_or.value().Valid());
}

TEST_F(BPlusTreeIteratorTest, SeekBeforeEveryKeyStartsAtTheFirst) {
    ASSERT_TRUE(tree_->Insert(Slice("m"), Slice("1")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("z"), Slice("2")).ok());
    auto it_or = tree_->Begin(Slice("a"));
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ToString(), "m");
}

TEST_F(BPlusTreeIteratorTest, NextPastEndReturnsInvalidArgument) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Next().ok());
    EXPECT_FALSE(it.Valid());
    Status s = it.Next();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(BPlusTreeIteratorTest, IteratorConstructedThenNeverAdvancedLeaksNoPinOnDestruction) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    page_id_t root_id = disk_manager_->GetRootPageId();
    {
        auto it_or = tree_->Begin();
        ASSERT_TRUE(it_or.ok());
        ASSERT_TRUE(it_or.value().Valid());
        EXPECT_EQ(bpm_->GetPinCount(root_id), 1u);
    }
    EXPECT_EQ(bpm_->GetPinCount(root_id), 0u);
}

TEST_F(BPlusTreeIteratorTest, ScanAfterDeletesSkipsRemovedKeysAndStaysSorted) {
    constexpr int kN = 150;
    for (int i = 0; i < kN; ++i) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }
    for (int i = 0; i < kN; i += 3) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Remove(Slice(key)).ok());
    }
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    int count = 0;
    std::string prev;
    while (it.Valid()) {
        std::string key = it.Key().ToString();
        if (!prev.empty()) {
            EXPECT_LT(prev, key);
        }
        prev = key;
        int idx = 0;
        std::sscanf(key.c_str(), "k%d", &idx);
        EXPECT_NE(idx % 3, 0) << "removed key " << key << " reappeared in scan";
        ++count;
        ASSERT_TRUE(it.Next().ok());
    }
    int expected_count = kN - (kN + 2) / 3;
    EXPECT_EQ(count, expected_count);
}

TEST_F(BPlusTreeIteratorTest, PropertyFullScanAlwaysMatchesMapOracleAfterRandomChurn) {
    std::map<std::string, std::string> oracle;
    std::mt19937 rng(321);
    for (int round = 0; round < 500; ++round) {
        bool do_insert = oracle.empty() || (rng() % 2 == 0);
        if (do_insert) {
            std::string key = "k" + std::to_string(rng() % 200);
            std::string value = "v" + std::to_string(round);
            Status s = tree_->Insert(Slice(key), Slice(value));
            if (s.ok()) {
                oracle[key] = value;
            }
        } else {
            auto it = oracle.begin();
            std::advance(it, rng() % oracle.size());

            ASSERT_TRUE(tree_->Remove(Slice(it->first)).ok());
            oracle.erase(it);
        }
    }

    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    auto oracle_it = oracle.begin();
    while (it.Valid()) {
        ASSERT_NE(oracle_it, oracle.end()) << "scan produced more keys than the oracle has";
        EXPECT_EQ(it.Key().ToString(), oracle_it->first);
        EXPECT_EQ(it.Value().ToString(), oracle_it->second);
        ++oracle_it;
        ASSERT_TRUE(it.Next().ok());
    }
    EXPECT_EQ(oracle_it, oracle.end()) << "scan ended before exhausting the oracle";
}

TEST_F(BPlusTreeIteratorTest, ScanSpansAContiguousMiddleDeletionCorrectly) {
    constexpr int kN = 150;
    for (int i = 0; i < kN; ++i) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v" + std::to_string(i))).ok());
    }
    for (int i = 50; i < 100; ++i) {
        std::string key = MakeKey(i);
        ASSERT_TRUE(tree_->Remove(Slice(key)).ok());
    }
    ASSERT_TRUE(tree_->Verify().ok());
    auto it_or = tree_->Begin();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    int count = 0;
    while (it.Valid()) {
        ++count;
        ASSERT_TRUE(it.Next().ok());
    }
    EXPECT_EQ(count, 100) << "scan lost keys across the gap. Likely a stale "
                             "next_leaf_page_id left over from a merge";
}

} // namespace
} // namespace engine
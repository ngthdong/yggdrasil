#include "engine/b_plus_tree.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <map>
#include <random>

namespace engine {
namespace {

class BPlusTreeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_bptree_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 256, true).value();
        bpm_ = std::make_unique<BufferPoolManager>(disk_manager_.get(), 8);
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

TEST_F(BPlusTreeTest, GetOnEmptyTreeReturnsNotFound) {
    EXPECT_TRUE(tree_->IsEmpty());
    auto v = tree_->Get(Slice("anything"));
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.status().code(), Status::Code::kNotFound);
}

TEST_F(BPlusTreeTest, InsertThenGetRoundTrips) {
    ASSERT_TRUE(tree_->Insert(Slice("hello"), Slice("world")).ok());
    auto v = tree_->Get(Slice("hello"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "world");
    EXPECT_FALSE(tree_->IsEmpty());
}

TEST_F(BPlusTreeTest, FirstInsertCreatesRootLeafExactlyOnce) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    page_id_t root1 = disk_manager_->GetRootPageId();
    ASSERT_TRUE(tree_->Insert(Slice("b"), Slice("2")).ok());
    page_id_t root2 = disk_manager_->GetRootPageId();
    EXPECT_EQ(root1, root2); // second insert must NOT allocate a new root
}

TEST_F(BPlusTreeTest, GetMissingKeyOnNonEmptyTreeReturnsNotFound) {
    ASSERT_TRUE(tree_->Insert(Slice("present"), Slice("v")).ok());
    auto v = tree_->Get(Slice("absent"));
    EXPECT_FALSE(v.ok());
    EXPECT_EQ(v.status().code(), Status::Code::kNotFound);
}

TEST_F(BPlusTreeTest, DuplicateInsertRejectedWithoutClobberingExistingValue) {
    ASSERT_TRUE(tree_->Insert(Slice("k"), Slice("original")).ok());
    Status s = tree_->Insert(Slice("k"), Slice("clobber-attempt"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    EXPECT_EQ(tree_->Get(Slice("k")).value(), "original");
}

TEST_F(BPlusTreeTest, PersistsAcrossReopen) {
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("b"), Slice("2")).ok());
    ASSERT_TRUE(tree_->Insert(Slice("c"), Slice("3")).ok());
    ASSERT_TRUE(bpm_->FlushAllPages().ok());
    ASSERT_TRUE(disk_manager_->Shutdown().ok());

    auto dm2 = DiskManager::Open(path_, 256, false).value();
    BufferPoolManager bpm2(dm2.get(), 8);
    FreePageManager fpm2(dm2.get(), &bpm2);
    BPlusTree tree2(dm2.get(), &bpm2, &fpm2);

    EXPECT_FALSE(tree2.IsEmpty());
    EXPECT_EQ(tree2.Get(Slice("a")).value(), "1");
    EXPECT_EQ(tree2.Get(Slice("b")).value(), "2");
    EXPECT_EQ(tree2.Get(Slice("c")).value(), "3");
}

TEST_F(BPlusTreeTest, PropertyRandomInsertMatchesMapOracle) {
    std::map<std::string, std::string> oracle;
    std::mt19937 rng(7);
    int successful_inserts = 0;

    for (int round = 0; round < 300; ++round) {
        std::string key = "k" + std::to_string(rng() % 200);
        std::string value = "v" + std::to_string(round);

        Status s = tree_->Insert(Slice(key), Slice(value));
        bool already_present = oracle.contains(key);

        if (already_present) {
            ASSERT_FALSE(s.ok());
            ASSERT_EQ(s.code(), Status::Code::kInvalidArgument);
        } else if (s.ok()) {
            oracle[key] = value;
            ++successful_inserts;
        } else {
            ASSERT_EQ(s.code(), Status::Code::kResourceExhausted);
        }
    }

    ASSERT_GT(successful_inserts, 0);
    for (const auto& [k, v] : oracle) {
        auto got = tree_->Get(Slice(k));
        ASSERT_TRUE(got.ok()) << "key " << k;
        EXPECT_EQ(got.value(), v);
    }
}

TEST_F(BPlusTreeTest, CustomComparatorIsActuallyUsedNotJustAccepted) {
    auto ci_compare = [](const Slice& a, const Slice& b) {
        std::string sa = a.ToString();
        std::string sb = b.ToString();
        for (auto& c : sa) {
            c = static_cast<char>(std::tolower(c));
        }
        for (auto& c : sb) {
            c = static_cast<char>(std::tolower(c));
        }
        if (sa.compare(sb) < 0) {
            return -1;
        }
        if (sa == sb) {
            return 0;
        }
        return 1;
    };
    BPlusTree ci_tree(disk_manager_.get(), bpm_.get(), fpm_.get(), ci_compare);

    ASSERT_TRUE(ci_tree.Insert(Slice("Hello"), Slice("v1")).ok());
    Status s = ci_tree.Insert(Slice("HELLO"), Slice("v2"));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    EXPECT_EQ(ci_tree.Get(Slice("hello")).value(), "v1");
}

} // namespace
} // namespace engine
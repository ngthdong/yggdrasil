#include "engine/bptree_leaf_page.h"

#include <gtest/gtest.h>
#include <map>
#include <random>
#include <vector>

namespace engine {
namespace {

constexpr uint32_t kPageSize = 256;

int ByteCompare(const Slice& a, const Slice& b) {
    return a.Compare(b);
}

class BPlusTreeLeafPageTest : public ::testing::Test {
  protected:
    void SetUp() override {
        buf_.assign(kPageSize, 0);
        BPlusTreeLeafPage::InitNewPage(buf_.data(), kPageSize, 1);
    }
    BPlusTreeLeafPage Page() {
        return {buf_.data(), kPageSize};
    }
    std::vector<char> buf_;
};

TEST_F(BPlusTreeLeafPageTest, FreshPageIsEmpty) {
    auto page = Page();
    EXPECT_EQ(page.page_id(), 1);
    EXPECT_EQ(page.num_keys(), 0);
    EXPECT_EQ(page.next_leaf_page_id(), kInvalidPageId);
}

TEST_F(BPlusTreeLeafPageTest, InsertThenFindReturnsCorrectValue) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("b"), Slice("value-b"), ByteCompare).ok());
    auto result = page.FindKey(Slice("b"), ByteCompare);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(page.ValueAt(result.index).ToString(), "value-b");
}

TEST_F(BPlusTreeLeafPageTest, InsertOutOfOrderKeepsDirectorySorted) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("c"), Slice("3"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("a"), Slice("1"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("b"), Slice("2"), ByteCompare).ok());

    ASSERT_EQ(page.num_keys(), 3);
    EXPECT_EQ(page.KeyAt(0).ToString(), "a");
    EXPECT_EQ(page.KeyAt(1).ToString(), "b");
    EXPECT_EQ(page.KeyAt(2).ToString(), "c");
    EXPECT_EQ(page.ValueAt(0).ToString(), "1");
    EXPECT_EQ(page.ValueAt(1).ToString(), "2");
    EXPECT_EQ(page.ValueAt(2).ToString(), "3");
}

TEST_F(BPlusTreeLeafPageTest, DuplicateKeyInsertIsRejected) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("x"), Slice("1"), ByteCompare).ok());
    Status s = page.Insert(Slice("x"), Slice("2"), ByteCompare);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    // Original value must be untouched.
    EXPECT_EQ(page.ValueAt(0).ToString(), "1");
}

TEST_F(BPlusTreeLeafPageTest, FindMissingKeyReturnsCorrectInsertionPoint) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("b"), Slice("2"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("d"), Slice("4"), ByteCompare).ok());

    auto before = page.FindKey(Slice("a"), ByteCompare);
    EXPECT_FALSE(before.found);
    EXPECT_EQ(before.index, 0); // "a" belongs before "b"

    auto middle = page.FindKey(Slice("c"), ByteCompare);
    EXPECT_FALSE(middle.found);
    EXPECT_EQ(middle.index, 1); // "c" belongs between "b" and "d"

    auto after = page.FindKey(Slice("e"), ByteCompare);
    EXPECT_FALSE(after.found);
    EXPECT_EQ(after.index, 2); // "e" belongs after "d"
}

TEST_F(BPlusTreeLeafPageTest, InsertUntilFullReturnsResourceExhausted) {
    auto page = Page();
    int count = 0;
    while (true) {
        std::string key = "k" + std::to_string(count);
        auto s = page.Insert(Slice(key), Slice("0123456789"), ByteCompare);
        if (!s.ok()) {
            EXPECT_EQ(s.code(), Status::Code::kResourceExhausted);
            break;
        }
        ++count;
        ASSERT_LT(count, 1000); // safety valve
    }
    EXPECT_GT(count, 0);
    for (int i = 0; i < count; ++i) {
        std::string key = "k" + std::to_string(i);
        auto result = page.FindKey(Slice(key), ByteCompare);
        EXPECT_TRUE(result.found) << "key " << key << " should still be present";
    }
}

TEST_F(BPlusTreeLeafPageTest, RemoveExistingKeySucceedsAndClosesGap) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("a"), Slice("1"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("b"), Slice("2"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("c"), Slice("3"), ByteCompare).ok());

    ASSERT_TRUE(page.Remove(Slice("b"), ByteCompare).ok());
    ASSERT_EQ(page.num_keys(), 2);
    EXPECT_EQ(page.KeyAt(0).ToString(), "a");
    EXPECT_EQ(page.KeyAt(1).ToString(), "c"); // "c" shifted left into "b"'s old slot
    auto found_b = page.FindKey(Slice("b"), ByteCompare);
    EXPECT_FALSE(found_b.found);
}

TEST_F(BPlusTreeLeafPageTest, RemoveMissingKeyReturnsNotFound) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("a"), Slice("1"), ByteCompare).ok());
    Status s = page.Remove(Slice("z"), ByteCompare);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kNotFound);
}

TEST_F(BPlusTreeLeafPageTest, RemoveAllKeysLeavesPageEmptyButUsable) {
    auto page = Page();
    ASSERT_TRUE(page.Insert(Slice("a"), Slice("1"), ByteCompare).ok());
    ASSERT_TRUE(page.Insert(Slice("b"), Slice("2"), ByteCompare).ok());
    ASSERT_TRUE(page.Remove(Slice("a"), ByteCompare).ok());
    ASSERT_TRUE(page.Remove(Slice("b"), ByteCompare).ok());
    EXPECT_EQ(page.num_keys(), 0);
    // Still usable afterward -- an empty page isn't a broken one.
    EXPECT_TRUE(page.Insert(Slice("c"), Slice("3"), ByteCompare).ok());
}

TEST_F(BPlusTreeLeafPageTest, IsUnderflowReflectsOccupancyNotJustKeyCount) {
    auto page = Page();
    EXPECT_TRUE(page.IsUnderflow()); // fresh empty page is well under half full
    int count = 0;
    while (page.IsUnderflow() && count < 100) {
        Status s =
            page.Insert(Slice("key" + std::to_string(count)), Slice("0123456789"), ByteCompare);
        if (!s.ok()) {
            break;
        }
        ++count;
    }
    EXPECT_FALSE(page.IsUnderflow());
}

TEST_F(BPlusTreeLeafPageTest, PropertyRandomInsertMatchesMapOracleAndStaysSorted) {
    auto page = Page();
    std::map<std::string, std::string> oracle;
    std::mt19937 rng(42);

    for (int round = 0; round < 200; ++round) {
        std::string key = "key" + std::to_string(rng() % 50);
        std::string value = "val" + std::to_string(round);

        auto s = page.Insert(Slice(key), Slice(value), ByteCompare);
        bool already_present = oracle.contains(key);
        if (already_present) {
            EXPECT_FALSE(s.ok());
            EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
        } else if (s.ok()) {
            oracle[key] = value;
        } else {
            EXPECT_EQ(s.code(), Status::Code::kResourceExhausted);
        }

        for (uint16_t i = 1; i < page.num_keys(); ++i) {
            ASSERT_LT(ByteCompare(page.KeyAt(static_cast<uint16_t>(i - 1)), page.KeyAt(i)), 0)
                << "directory not sorted after round " << round;
        }
    }

    EXPECT_EQ(page.num_keys(), static_cast<uint16_t>(oracle.size()));
    for (const auto& [k, v] : oracle) {
        auto result = page.FindKey(Slice(k), ByteCompare);
        ASSERT_TRUE(result.found);
        EXPECT_EQ(page.ValueAt(result.index).ToString(), v);
    }
}

} // namespace
} // namespace engine
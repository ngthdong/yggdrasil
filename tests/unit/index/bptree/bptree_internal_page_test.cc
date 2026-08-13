#include <gtest/gtest.h>
#include <vector>

#include "engine/bptree_internal_page.h"

namespace engine {
namespace {

constexpr uint32_t kPageSize = 256;

int ByteCompare(const Slice& a, const Slice& b) {
    return a.Compare(b);
}

class BPlusTreeInternalPageTest : public ::testing::Test {
  protected:
    void SetUp() override {
        buf_.assign(kPageSize, 0);
        // leftmost_child_id = 100, an arbitrary "child for keys < everything"
        BPlusTreeInternalPage::InitNewPage(buf_.data(), kPageSize, 1, 100);
    }
    BPlusTreeInternalPage Page() {
        return {buf_.data(), kPageSize};
    }
    std::vector<char> buf_;
};

TEST_F(BPlusTreeInternalPageTest, FreshPageHasZeroKeysAndOnlyLeftmostChild) {
    auto page = Page();
    EXPECT_EQ(page.num_keys(), 0);
    EXPECT_EQ(page.ChildAt(0), 100);
}

TEST_F(BPlusTreeInternalPageTest, SingleEntryGivesTwoChildrenOneKey) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    EXPECT_EQ(page.num_keys(), 1);
    EXPECT_EQ(page.ChildAt(0), 100); // unchanged leftmost
    EXPECT_EQ(page.ChildAt(1), 200); // the new entry's child
    EXPECT_EQ(page.KeyAt(0).ToString(), "m");
}

TEST_F(BPlusTreeInternalPageTest, FindChildIndexBelowAllKeysReturnsZero) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    ASSERT_TRUE(page.InsertEntry(1, Slice("t"), 300).ok());
    EXPECT_EQ(page.FindChildIndex(Slice("a"), ByteCompare), 0);
}

TEST_F(BPlusTreeInternalPageTest, FindChildIndexEqualToSeparatorGoesRight) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    EXPECT_EQ(page.FindChildIndex(Slice("m"), ByteCompare), 1); // not 0
}

TEST_F(BPlusTreeInternalPageTest, FindChildIndexAboveAllKeysReturnsLast) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    ASSERT_TRUE(page.InsertEntry(1, Slice("t"), 300).ok());
    EXPECT_EQ(page.FindChildIndex(Slice("z"), ByteCompare), 2);
    EXPECT_EQ(page.ChildAt(2), 300);
}

TEST_F(BPlusTreeInternalPageTest, FindChildIndexBetweenKeysReturnsMiddle) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    ASSERT_TRUE(page.InsertEntry(1, Slice("t"), 300).ok());
    EXPECT_EQ(page.FindChildIndex(Slice("q"), ByteCompare), 1);
    EXPECT_EQ(page.ChildAt(1), 200);
}

TEST_F(BPlusTreeInternalPageTest, InsertAtBeginningShiftsExistingEntriesRight) {
    auto page = Page();
    ASSERT_TRUE(page.InsertEntry(0, Slice("m"), 200).ok());
    ASSERT_TRUE(page.InsertEntry(0, Slice("a"), 50).ok()); // insert BEFORE "m"

    ASSERT_EQ(page.num_keys(), 2);
    EXPECT_EQ(page.KeyAt(0).ToString(), "a");
    EXPECT_EQ(page.KeyAt(1).ToString(), "m");
    EXPECT_EQ(page.ChildAt(0), 100); // leftmost, unchanged
    EXPECT_EQ(page.ChildAt(1), 50);  // "a"'s child
    EXPECT_EQ(page.ChildAt(2), 200); // "m"'s child, shifted from position 1
}

TEST_F(BPlusTreeInternalPageTest, EveryChildReachableAfterMultipleInserts) {
    auto page = Page();
    std::vector<std::pair<std::string, page_id_t>> inserted; // (key, child) in insertion order
    std::vector<std::string> keys_in_order = {"d", "b", "f", "a"};
    page_id_t next_child = 10;
    for (const auto& k : keys_in_order) {
        uint16_t idx = page.FindChildIndex(Slice(k), ByteCompare);
        ASSERT_TRUE(page.InsertEntry(idx, Slice(k), next_child).ok());
        ++next_child;
    }
    // Keys must now be sorted: a, b, d, f
    ASSERT_EQ(page.num_keys(), 4);
    EXPECT_EQ(page.KeyAt(0).ToString(), "a");
    EXPECT_EQ(page.KeyAt(1).ToString(), "b");
    EXPECT_EQ(page.KeyAt(2).ToString(), "d");
    EXPECT_EQ(page.KeyAt(3).ToString(), "f");
    // 5 children total (leftmost + 4), every one must be independently reachable.
    EXPECT_EQ(page.ChildAt(0), 100);
    EXPECT_EQ(page.FindChildIndex(Slice("a"), ByteCompare), 1);
    EXPECT_EQ(page.FindChildIndex(Slice("c"), ByteCompare), 2);
    EXPECT_EQ(page.FindChildIndex(Slice("e"), ByteCompare), 3);
    EXPECT_EQ(page.FindChildIndex(Slice("g"), ByteCompare), 4);
}

TEST_F(BPlusTreeInternalPageTest, InsertUntilFullReturnsResourceExhausted) {
    auto page = Page();
    int count = 0;
    while (true) {
        std::string key = "k" + std::to_string(1000 + count); // fixed-width, keeps ordering simple
        auto s = page.InsertEntry(page.num_keys(), Slice(key), 500 + count);
        if (!s.ok()) {
            EXPECT_EQ(s.code(), Status::Code::kResourceExhausted);
            break;
        }
        ++count;
        ASSERT_LT(count, 1000);
    }
    EXPECT_GT(count, 0);
}

} // namespace
} // namespace engine
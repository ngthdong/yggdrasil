#include "engine/slotted_page.h"

#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <vector>

namespace engine {
namespace {

constexpr uint32_t kPageSize = 256;

class SlottedPageTest : public ::testing::Test {
  protected:
    void SetUp() override {
        buf_.assign(kPageSize, 0);
        SlottedPage::InitNewPage(buf_.data(), kPageSize, /*page_id=*/1);
    }

    SlottedPage Page() {
        return {buf_.data(), kPageSize};
    }

    std::vector<char> buf_;
};

TEST_F(SlottedPageTest, FreshPageIsEmpty) {
    SlottedPage page = Page();
    EXPECT_EQ(page.page_id(), 1);
    EXPECT_EQ(page.num_slots(), 0);
    EXPECT_EQ(page.free_space_offset(), kPageSize);
    EXPECT_EQ(page.FreeSpaceContiguous(), kPageSize - SlottedPage::kHeaderSize);
}

TEST_F(SlottedPageTest, InsertThenGetRoundTrips) {
    SlottedPage page = Page();
    auto slot_or = page.InsertRecord(Slice("hello"));
    ASSERT_TRUE(slot_or.ok());
    auto rec_or = page.GetRecord(slot_or.value());
    ASSERT_TRUE(rec_or.ok());
    EXPECT_EQ(rec_or.value().ToString(), "hello");
}

TEST_F(SlottedPageTest, MultipleInsertsGetDistinctStableSlotIds) {
    SlottedPage page = Page();
    auto s0 = page.InsertRecord(Slice("aaa")).value();
    auto s1 = page.InsertRecord(Slice("bb")).value();
    auto s2 = page.InsertRecord(Slice("c")).value();
    EXPECT_EQ(s0, 0);
    EXPECT_EQ(s1, 1);
    EXPECT_EQ(s2, 2);
    EXPECT_EQ(page.GetRecord(s0).value().ToString(), "aaa");
    EXPECT_EQ(page.GetRecord(s1).value().ToString(), "bb");
    EXPECT_EQ(page.GetRecord(s2).value().ToString(), "c");
}

TEST_F(SlottedPageTest, DeleteThenGetReturnsNotFound) {
    SlottedPage page = Page();
    auto s0 = page.InsertRecord(Slice("victim")).value();
    ASSERT_TRUE(page.DeleteRecord(s0).ok());
    auto rec_or = page.GetRecord(s0);
    EXPECT_FALSE(rec_or.ok());
    EXPECT_EQ(rec_or.status().code(), Status::Code::kNotFound);
}

TEST_F(SlottedPageTest, DoubleDeleteReturnsNotFound) {
    SlottedPage page = Page();
    auto s0 = page.InsertRecord(Slice("x")).value();
    ASSERT_TRUE(page.DeleteRecord(s0).ok());
    Status s = page.DeleteRecord(s0);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kNotFound);
}

TEST_F(SlottedPageTest, InsertUntilFullReturnsResourceExhausted) {
    SlottedPage page = Page();
    int count = 0;
    StatusOr<slot_id_t> last = Status::OK(); // placeholder, overwritten below
    while (true) {
        auto s = page.InsertRecord(Slice("0123456789")); // 10 bytes + 5-byte slot = 15/insert
        if (!s.ok()) {
            EXPECT_EQ(s.status().code(), Status::Code::kResourceExhausted);
            break;
        }
        ++count;
        ASSERT_LT(count, 1000); // safety valve against an infinite loop bug
    }
    EXPECT_GT(count, 0);
}

TEST_F(SlottedPageTest, CompactReclaimsTombstonedSpace) {
    SlottedPage page = Page();
    // Fill the page with same-size records, delete every other one, confirm
    // a record that only fits after reclaiming tombstoned space now succeeds.
    std::vector<slot_id_t> slots;
    while (true) {
        auto s = page.InsertRecord(Slice("0123456789"));
        if (!s.ok()) {
            break;
        }
        slots.push_back(s.value());
    }
    ASSERT_GE(slots.size(), 4u);

    for (size_t i = 0; i < slots.size(); i += 2) {
        ASSERT_TRUE(page.DeleteRecord(slots[i]).ok());
    }

    uint32_t before_contig = page.FreeSpaceContiguous();
    uint32_t reclaimable = page.FreeSpaceReclaimable();
    EXPECT_GT(reclaimable, before_contig);

    // This insert should fail contiguously but succeed via auto-compaction.
    auto s = page.InsertRecord(Slice("0123456789"));
    EXPECT_TRUE(s.ok()) << "expected auto-compaction to make room";
}

TEST_F(SlottedPageTest, SlotIdStableAcrossCompactionOfOtherSlots) {
    SlottedPage page = Page();
    auto s0 = page.InsertRecord(Slice("keep-me-0")).value();
    auto s1 = page.InsertRecord(Slice("delete-me")).value();
    auto s2 = page.InsertRecord(Slice("keep-me-2")).value();

    ASSERT_TRUE(page.DeleteRecord(s1).ok());
    page.Compact();

    // s0 and s2's slot_id and contents must be unchanged even though their
    // physical offsets moved during Compact().
    EXPECT_EQ(page.GetRecord(s0).value().ToString(), "keep-me-0");
    EXPECT_EQ(page.GetRecord(s2).value().ToString(), "keep-me-2");
    // s1 remains permanently invalid
    // slot_ids are never reused.
    EXPECT_FALSE(page.GetRecord(s1).ok());
}

TEST_F(SlottedPageTest, CorruptedSlotOffsetIsDetectedNotCrashed) {
    SlottedPage page = Page();
    auto s0 = page.InsertRecord(Slice("normal record")).value();

    // Reach past the API and directly corrupt slot 0's stored offset to
    // point at garbage, simulating a bit-flipped/corrupted page on disk.
    uint32_t slot_off = SlottedPage::kHeaderSize + s0 * SlottedPage::kSlotEntrySize;
    buf_[slot_off] = static_cast<char>(0xFF);
    buf_[slot_off + 1] = static_cast<char>(0xFF); // offset now ~65535, way OOB

    auto rec_or = page.GetRecord(s0);
    EXPECT_FALSE(rec_or.ok());
    EXPECT_EQ(rec_or.status().code(), Status::Code::kCorruption);
}

TEST_F(SlottedPageTest, PropertyRandomInsertDeleteMatchesMapOracle) {
    SlottedPage page = Page();
    std::map<slot_id_t, std::string> oracle;
    std::mt19937 rng(7);

    for (int round = 0; round < 500; ++round) {
        int op = static_cast<int>(rng() % 3);
        if (op == 0 || oracle.empty()) {
            // Insert a random-length record (kept small so many fit in 256 bytes).
            size_t len = 1 + (rng() % 20);
            std::string data;
            for (size_t i = 0; i < len; ++i) {
                data.push_back(static_cast<char>('a' + (rng() % 26)));
            }

            auto s = page.InsertRecord(Slice(data));
            if (s.ok()) {
                oracle[s.value()] = data;
            } else {
                EXPECT_EQ(s.status().code(), Status::Code::kResourceExhausted);
            }
        } else if (op == 1 && !oracle.empty()) {
            // Delete a random existing (live) slot.
            auto it = oracle.begin();
            std::advance(it, rng() % oracle.size());
            ASSERT_TRUE(page.DeleteRecord(it->first).ok());
            oracle.erase(it);
        } else {
            // Trigger a compaction directly, at random, and confirm nothing
            // that should still be live gets lost or corrupted by it.
            page.Compact();
        }

        // Full cross-check every round
        for (const auto& [slot, expected] : oracle) {
            auto rec_or = page.GetRecord(slot);
            ASSERT_TRUE(rec_or.ok()) << "slot " << slot << " should be live";
            EXPECT_EQ(rec_or.value().ToString(), expected)
                << "slot " << slot << " content mismatch";
        }
    }
}

TEST_F(SlottedPageTest, RandomByteCorruptionNeverCrashesOnlyReportsStatus) {
    // A lightweight, in-process stand-in for a libFuzzer harness: repeatedly
    // build a valid page, flip random bytes in the slot directory, and
    // confirm every accessor either succeeds correctly or returns Corruption
    // never garbage, never a crash, never a hang. Run under ASan in CI
    // so an out-of-bounds read would be caught even if this bounds-check
    // logic itself has a bug.
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<char> buf(kPageSize, 0);
        SlottedPage::InitNewPage(buf.data(), kPageSize, 1);
        SlottedPage page(buf.data(), kPageSize);

        int n = 1 + static_cast<int>(rng() % 5);
        std::vector<slot_id_t> slots;
        for (int i = 0; i < n; ++i) {
            auto s = page.InsertRecord(Slice("abcdefgh"));
            if (s.ok()) {
                slots.push_back(s.value());
            }
        }
        if (slots.empty()) {
            continue;
        }

        // Flip a random byte somewhere in the slot directory region.
        uint32_t dir_start = SlottedPage::kHeaderSize;
        uint32_t dir_end =
            dir_start + static_cast<uint32_t>(slots.size()) * SlottedPage::kSlotEntrySize;
        uint32_t byte_idx = dir_start + static_cast<uint32_t>(rng() % (dir_end - dir_start));
        buf[byte_idx] = static_cast<char>(rng() % 256);

        for (slot_id_t s : slots) {
            auto rec_or = page.GetRecord(s);
            // No crash reaching this line is itself the primary assertion; we
            // additionally require the result to be a well-formed Status either way.
            if (!rec_or.ok()) {
                EXPECT_TRUE(rec_or.status().code() == Status::Code::kCorruption ||
                            rec_or.status().code() == Status::Code::kNotFound);
            }
        }
    }
}

} // namespace
} // namespace engine
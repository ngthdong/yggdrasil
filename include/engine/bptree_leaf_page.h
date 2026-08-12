#pragma once

#include <cstdint>
#include <functional>

#include "engine/config.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// Separate from SlottedPage because their directory invariants differ:
// SlottedPage preserves stable slot_ids, while BPlusTreeLeafPage keeps
// entries sorted by key for binary search and may reorder indices on insert.
//
// Layout (little-endian, hand-packed via byte_utils):
//   [0:4)   page_id            (i32)
//   [4:5)   page_type          (u8)  - PageType::kBTreeLeaf
//   [5:7)   num_keys           (u16)
//   [7:9)   free_space_offset  (u16) - record area is [free_space_offset, page_size)
//   [9:13)  next_leaf_page_id  (i32) - kInvalidPageId wires the leaf-linked-list scan
//   [13:17) checksum           (u32) - reserved
//   [17: )  key_directory[num_keys], each entry 2 bytes: offset (u16) into
//           the record area, SORTED BY KEY, not by insertion order.
//
// Record area, each record:
//   [0:2)            key_length   (u16)
//   [2:2+kl)         key bytes
//   [2+kl:4+kl)      value_length (u16)
//   [4+kl:4+kl+vl)   value bytes
class BPlusTreeLeafPage {
  public:
    using KeyComparator = std::function<int(const Slice&, const Slice&)>;

    static constexpr uint32_t kHeaderSize = 17;
    static constexpr uint32_t kDirEntrySize = 2;

    struct SearchResult {
        uint16_t index;
        bool found;
    };

    static void InitNewPage(char* buf, uint32_t page_size, page_id_t page_id);

    BPlusTreeLeafPage(char* buf, uint32_t page_size) : buf_(buf), page_size_(page_size) {}

    page_id_t page_id() const;
    uint16_t num_keys() const;
    uint16_t free_space_offset() const;
    page_id_t next_leaf_page_id() const;
    void SetNextLeafPageId(page_id_t next);
    uint32_t FreeSpaceContiguous() const;

    // Binary search over the sorted key directory.
    SearchResult FindKey(const Slice& key, const KeyComparator& cmp) const;

    Slice KeyAt(uint16_t index) const;
    Slice ValueAt(uint16_t index) const;

    Status Insert(const Slice& key, const Slice& value, const KeyComparator& cmp);

    Status Remove(const Slice& key, const KeyComparator& cmp);

    // Rebuilds the page from live entries, reclaiming dead space left by
    // Remove()/redistribution. Insert() invokes this when contiguous free space
    // is insufficient. Live occupancy must be computed from actual records,
    // not free_space_offset, which includes stale dead space
    void Compact();

    uint32_t OccupiedBytes() const {
        uint32_t total = kHeaderSize + num_keys() * kDirEntrySize;
        for (uint16_t i = 0; i < num_keys(); ++i) {
            total += 2 + static_cast<uint32_t>(KeyAt(i).size()) + 2 +
                     static_cast<uint32_t>(ValueAt(i).size());
        }
        return total;
    }
    bool IsUnderflow() const {
        return OccupiedBytes() < page_size_ / 2;
    }

  private:
    // Appends a record directly at the end of the directory with NO
    // search, no duplicate check, and no shift.
    // Used only by Compact().
    void AppendRecordUnchecked(const Slice& key, const Slice& value);

    uint32_t DirEntryOffset(uint16_t index) const {
        return kHeaderSize + index * kDirEntrySize;
    }
    uint32_t DirEnd() const {
        return kHeaderSize + num_keys() * kDirEntrySize;
    }

    void set_num_keys(uint16_t n);
    void set_free_space_offset(uint16_t off);

    char* buf_;
    uint32_t page_size_;
};

} // namespace engine
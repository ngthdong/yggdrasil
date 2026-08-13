#pragma once

#include <cstdint>
#include <functional>

#include "engine/config.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// n keys, n+1 children. The leftmost child is its own named field, and
// every other child is paired with the key immediately to its left in a
// single shifting array. There is no way to accidentally treat
// "children" and "keys" as same-length parallel arrays, because they
// aren't.
//
// Layout:
//   [0:4)   page_id            (i32)
//   [4:5)   page_type          (u8)  - PageType::kBTreeInternal
//   [5:7)   num_keys           (u16)
//   [7:9)   free_space_offset  (u16)
//   [9:13)  leftmost_child_id  (i32) - the child for keys < KeyAt(0)
//   [13:17) checksum           (u32) - reserved
//   [17: )  entries[num_keys], each entry 6 bytes:
//              key_offset (u16), child_id (i32)
//           entries[i].child_id is the child for keys in
//           [KeyAt(i), KeyAt(i+1)).
class BPlusTreeInternalPage {
  public:
    using KeyComparator = std::function<int(const Slice&, const Slice&)>;

    static constexpr uint32_t kHeaderSize = 17;
    static constexpr uint32_t kEntrySize = 6;

    static void
    InitNewPage(char* buf, uint32_t page_size, page_id_t page_id, page_id_t leftmost_child_id);

    BPlusTreeInternalPage(char* buf, uint32_t page_size) : buf_(buf), page_size_(page_size) {}

    page_id_t page_id() const;
    uint16_t num_keys() const;
    uint16_t free_space_offset() const;
    Slice KeyAt(uint16_t index) const;
    page_id_t ChildAt(uint16_t index) const;
    uint32_t FreeSpaceContiguous() const;

    // Returns the count of KeyAt(i) <= key, an upper-bound search, both
    // Get's traversal and Insert's split propagation call this.
    uint16_t FindChildIndex(const Slice& key, const KeyComparator& cmp) const;

    // child_id becomes ChildAt(index+1), key becomes KeyAt(index).
    Status InsertEntry(uint16_t index, const Slice& key, page_id_t child_id);

    uint32_t OccupiedBytes() const {
        uint32_t total = kHeaderSize + num_keys() * kEntrySize;
        for (uint16_t i = 0; i < num_keys(); ++i) {
            total += 2 + static_cast<uint32_t>(KeyAt(i).size());
        }
        return total;
    }
    bool IsUnderflow() const {
        return OccupiedBytes() < page_size_ / 2;
    }

  private:
    uint32_t EntryOffset(uint16_t index) const {
        return kHeaderSize + index * kEntrySize;
    }
    uint32_t DirEnd() const {
        return kHeaderSize + num_keys() * kEntrySize;
    }
    void set_num_keys(uint16_t n);
    void set_free_space_offset(uint16_t off);
    void set_leftmost_child_id(page_id_t id);

    char* buf_;
    uint32_t page_size_;
};

} // namespace engine
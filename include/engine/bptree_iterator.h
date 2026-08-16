#pragma once

#include "engine/buffer_pool_manager.h"
#include "engine/config.h"
#include "engine/page_guard.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

class BPlusTree;

// Forward-only iterator over the B+Tree's linked leaf pages.
//
// The current leaf page stays pinned while the iterator is valid.
// Key() and Value() return Slices that point directly to this page,
// so they are valid only until the iterator advances or is destroyed.
//
// Unlike BPlusTree::Get(), the iterator does not copy the data.
// Keeping the page pinned allows efficient sequential/range scans.
class BPlusTreeIterator {
  public:
    BPlusTreeIterator() = default;

    BPlusTreeIterator(BPlusTreeIterator&&) = default;
    BPlusTreeIterator& operator=(BPlusTreeIterator&&) = default;

    BPlusTreeIterator(const BPlusTreeIterator&) = delete;
    BPlusTreeIterator& operator=(const BPlusTreeIterator&) = delete;

    bool Valid() const {
        return guard_.is_valid();
    }

    Slice Key();
    Slice Value();
    Status Next();

  private:
    friend class BPlusTree;
    BPlusTreeIterator(BufferPoolManager* bpm, PageGuard guard, uint16_t slot_index)
        : bpm_(bpm), guard_(std::move(guard)), slot_index_(slot_index) {}

    // Moves the iterator to the next leaf when the current leaf is exhausted.
    // If there is no next leaf, releases the guard and marks the iterator
    // invalid.
    Status AdvancePastEndIfNeeded();

    BufferPoolManager* bpm_ = nullptr;
    PageGuard guard_;
    uint16_t slot_index_ = 0;
};

} // namespace engine
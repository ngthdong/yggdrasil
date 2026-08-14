#pragma once

#include "engine/buffer_pool_manager.h"
#include "engine/config.h"
#include "engine/page_guard.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

class BPlusTree;

// A forward-only cursor over the B+Tree's leaf-linked-list, built on the
// next_leaf_page_id chain Stage 7's splits and Stage 8's merges have
// been maintaining since before this stage ever read it. Holds exactly
// one leaf page pinned at a time.
//
// IMPORTANT, and a deliberate contrast with BPlusTree::Get(): Key()/
// Value() return a Slice that points directly into the pinned leaf page,
// valid until the NEXT call to Next() (or the iterator's destruction).
// Get() unpins before returning and therefore must copy to a std::string
// to stay safe; an iterator does the opposite on purpose -- it keeps its
// page pinned specifically so callers can read Key()/Value() repeatedly
// without paying a copy each time. Mirrors LevelDB/RocksDB's contract.
class BPlusTreeIterator {
  public:
    BPlusTreeIterator() = default; // an invalid ("end") iterator

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

    Status AdvancePastEndIfNeeded();

    BufferPoolManager* bpm_ = nullptr;
    PageGuard guard_;
    uint16_t slot_index_ = 0;
};

} // namespace engine
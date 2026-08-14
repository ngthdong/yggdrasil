#pragma once

#include <functional>
#include <string>

#include "engine/bptree_internal_page.h"
#include "engine/bptree_iterator.h"
#include "engine/bptree_leaf_page.h"
#include "engine/buffer_pool_manager.h"
#include "engine/disk_manager.h"
#include "engine/free_page_manager.h"
#include "engine/page_guard.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// The tree consists of a single leaf page; no split, delete, internal
// pages, or iterator. Full-leaf inserts return ResourceExhausted.
// Get() returns a string copy because the page is unpinned before return,
// making a returned Slice invalid.
class BPlusTree {
  public:
    using Iterator = BPlusTreeIterator;
    using KeyComparator = std::function<int(const Slice&, const Slice&)>;
    static int DefaultComparator(const Slice& a, const Slice& b) {
        return a.Compare(b);
    }

    BPlusTree(DiskManager* disk_manager,
              BufferPoolManager* buffer_pool_manager,
              FreePageManager* free_page_manager,
              KeyComparator comparator = DefaultComparator)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager),
          free_page_manager_(free_page_manager), comparator_(std::move(comparator)) {}

    bool IsEmpty() const {
        return disk_manager_->GetRootPageId() == kInvalidPageId;
    }

    StatusOr<std::string> Get(const Slice& key);
    Status Insert(const Slice& key, const Slice& value);
    Status Remove(const Slice& key);

    StatusOr<Iterator> Begin();
    StatusOr<Iterator> Begin(const Slice& start_key);

    StatusOr<int> Height();
    Status Verify();
    StatusOr<std::string> ToString();

  private:
    struct InsertResult {
        bool split_occurred = false;
        std::string split_key;
        page_id_t new_right_child_id = kInvalidPageId;
    };

    struct RebalanceResult {
        bool merged = false;       // true: everything ended up in the LEFT page, RIGHT was emptied
        std::string new_separator; // meaningful only if !merged
    };

    StatusOr<page_id_t> GetOrCreateRootLeaf();

    StatusOr<page_id_t> DescendToLeaf(const Slice* key);

    StatusOr<InsertResult> InsertRecursive(page_id_t page_id, const Slice& key, const Slice& value);

    StatusOr<InsertResult> SplitLeafAndInsert(PageGuard& leaf_guard,
                                              page_id_t page_id,
                                              const Slice& key,
                                              const Slice& value);

    StatusOr<InsertResult> SplitInternalAndInsert(PageGuard& internal_guard,
                                                  page_id_t page_id,
                                                  uint16_t insert_idx,
                                                  const Slice& new_key,
                                                  page_id_t new_child_id);

    Status RemoveRecursive(page_id_t page_id, const Slice& key);

    Status MaybeRebalanceChild(PageGuard& parent_guard, uint16_t child_idx);

    StatusOr<RebalanceResult> RebalanceLeafPair(PageGuard& left_guard,
                                                page_id_t left_id,
                                                PageGuard& right_guard,
                                                page_id_t right_id);

    StatusOr<RebalanceResult> RebalanceInternalPair(PageGuard& left_guard,
                                                    page_id_t left_id,
                                                    PageGuard& right_guard,
                                                    page_id_t right_id,
                                                    const Slice& parent_separator);

    Status VerifyRecursive(page_id_t page_id,
                           const Slice* min_key,
                           const Slice* max_key,
                           int depth,
                           int* out_leaf_depth);

    StatusOr<std::string> ToStringRecursive(page_id_t page_id, int depth);

    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    FreePageManager* free_page_manager_;
    KeyComparator comparator_;
};

} // namespace engine
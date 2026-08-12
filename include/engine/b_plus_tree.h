#pragma once

#include <functional>
#include <string>

#include "engine/bptree_leaf_page.h"
#include "engine/buffer_pool_manager.h"
#include "engine/disk_manager.h"
#include "engine/free_page_manager.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// The tree consists of a single leaf page; no split, delete, internal
// pages, or iterator. Full-leaf inserts return ResourceExhausted.
// Get() returns a string copy because the page is unpinned before return,
// making a returned Slice invalid.
class BPlusTree {
  public:
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

  private:
    StatusOr<page_id_t> GetOrCreateRootLeaf();

    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    FreePageManager* free_page_manager_;
    KeyComparator comparator_;
};

} // namespace engine
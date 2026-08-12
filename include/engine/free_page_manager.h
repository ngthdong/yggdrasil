#pragma once

#include <vector>

#include "engine/buffer_pool_manager.h"
#include "engine/config.h"
#include "engine/disk_manager.h"
#include "engine/status.h"

namespace engine {

// FreePageManager gives deleted pages a second life instead of letting the
// file grow monotonically forever. Deallocated pages become a singly-
// linked list rooted at the superblock's free_list_head_page_id.
//
// This class is a client of BOTH DiskManager (extending the file when the
// free list is empty; reading/writing the superblock's head pointer) and
// BufferPoolManager (fetching/unpinning free-list node pages through the
// SAME cache as any other page).
class FreePageManager {
  public:
    FreePageManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager) {}

    StatusOr<page_id_t> AllocatePage();
    StatusOr<char*> AllocateAndPinPage(page_id_t* out_page_id);

    // page_id must have pin_count == 0. Also rejects page_id already sitting
    // at the free list head as a cheap (not exhaustive) double-free guard.
    Status DeallocatePage(page_id_t page_id);

    page_id_t FreeListHead() const {
        return disk_manager_->GetFreeListHead();
    }

    StatusOr<std::vector<page_id_t>> DebugWalkFreeList(size_t max_length = 1'000'000) const;

  private:
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
};

} // namespace engine
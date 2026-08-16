#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "engine/config.h"
#include "engine/disk_manager.h"
#include "engine/replacer.h"
#include "engine/status.h"
#include "engine/wal_manager.h"

namespace engine {

enum class ReplacerPolicy {
    kClock,
    kLRU,
};

// BufferPoolManager caches pages in a fixed-size array of frames, sitting
// between the B+Tree and DiskManager. It owns all page memory from this
// point forward, callers never allocate pagebytes themselves, they only ever
// get a pointer into a frame this class owns, valid for exactly as long as
// they hold a pin on it.
class BufferPoolManager {
  public:
    BufferPoolManager(DiskManager* disk_manager,
                      size_t num_frames,
                      ReplacerPolicy policy = ReplacerPolicy::kClock);

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Pins and returns a pointer to page_id's bytes from cache if
    // resident, otherwise read from disk into a free frame. Fails with
    // ResourceExhausted if not resident and no free frame exists.
    StatusOr<char*> FetchPage(page_id_t page_id);

    // Allocates a brand-new page via DiskManager, pins it, returns a
    // pointer to its (zeroed) bytes. *out_page_id receives the new id.
    StatusOr<char*> NewPage(page_id_t* out_page_id);

    // Claims a frame for a page_id the CALLER already determined
    StatusOr<char*> NewPageWithId(page_id_t page_id);

    // Decrements pin count. is_dirty is sticky-OR'd onto the frame's dirty
    // flag. When pin_count reaches exactly 0, the frame becomes evictable
    Status UnpinPage(page_id_t page_id, bool is_dirty);

    // Writes to disk iff dirty, then clears the flag. Safe with pin_count==0.
    Status FlushPage(page_id_t page_id);
    Status FlushAllPages();

    void SetWalManager(WalManager* wal_manager) {
        wal_manager_ = wal_manager;
    }
    Status SetPageLSN(page_id_t page_id, lsn_t lsn);

    size_t GetPinCount(page_id_t page_id) const;
    bool IsResident(page_id_t page_id) const;
    lsn_t GetPageLSN(page_id_t page_id) const;
    size_t FreeFrameCount() const {
        return free_list_.size();
    }
    size_t EvictableFrameCount() const {
        return replacer_->Size();
    }
    size_t CapacityFrames() const {
        return frames_.size();
    }
    uint32_t page_size() const {
        return page_size_;
    }
    uint64_t HitCount() const {
        return hit_count_;
    }
    uint64_t MissCount() const {
        return miss_count_;
    }
    std::string DebugString() const;

  private:
    struct Frame {
        page_id_t page_id = kInvalidPageId;
        size_t pin_count = 0;
        bool is_dirty = false;
        lsn_t page_lsn = kInvalidLsn;
        char* data = nullptr;
    };

    StatusOr<frame_id_t> GetFreeFrame();
    StatusOr<char*> ClaimFrameForNewPage(page_id_t page_id);

    DiskManager* disk_manager_; // not owned
    uint32_t page_size_;
    std::vector<char> pool_memory_;
    std::vector<Frame> frames_;
    std::vector<frame_id_t> free_list_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::unique_ptr<Replacer> replacer_;
    uint64_t hit_count_ = 0;
    uint64_t miss_count_ = 0;
    WalManager* wal_manager_ = nullptr; // not owned
};

} // namespace engine
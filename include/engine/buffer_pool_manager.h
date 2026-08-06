#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "engine/config.h"
#include "engine/disk_manager.h"
#include "engine/status.h"

namespace engine {

// BufferPoolManager caches pages in a fixed-size array of frames, sitting
// between the B+Tree and DiskManager. It owns all page memory from this
// point forward, callers never allocate pagebytes themselves, they only ever
// get a pointer into a frame this class owns, valid for exactly as long as
// they hold a pin on it.
class BufferPoolManager {
  public:
    BufferPoolManager(DiskManager* disk_manager, size_t num_frames);

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Pins and returns a pointer to page_id's bytes from cache if
    // resident, otherwise read from disk into a free frame. Fails with
    // ResourceExhausted if not resident and no free frame exists.
    StatusOr<char*> FetchPage(page_id_t page_id);

    // Allocates a brand-new page via DiskManager, pins it, returns a
    // pointer to its (zeroed) bytes. *out_page_id receives the new id.
    StatusOr<char*> NewPage(page_id_t* out_page_id);

    // Decrements pin count. is_dirty is sticky-OR'd onto the frame's dirty
    // flag -- once true, stays true until the next successful FlushPage.
    // InvalidArgument if not resident, or if pin_count is already 0.
    Status UnpinPage(page_id_t page_id, bool is_dirty);

    // Writes to disk iff dirty, then clears the flag. Safe with pin_count==0.
    Status FlushPage(page_id_t page_id);
    Status FlushAllPages();

    size_t GetPinCount(page_id_t page_id) const;
    bool IsResident(page_id_t page_id) const;
    size_t FreeFrameCount() const {
        return free_list_.size();
    }
    size_t CapacityFrames() const {
        return frames_.size();
    }
    uint32_t page_size() const {
        return page_size_;
    }
    std::string DebugString() const;

  private:
    using frame_id_t = int32_t;

    struct Frame {
        page_id_t page_id = kInvalidPageId;
        size_t pin_count = 0;
        bool is_dirty = false;
        char* data = nullptr;
    };

    DiskManager* disk_manager_; // not owned
    uint32_t page_size_;
    std::vector<char> pool_memory_;
    std::vector<Frame> frames_;
    std::vector<frame_id_t> free_list_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
};

} // namespace engine
#pragma once

#include <memory>
#include <string>

#include "engine/config.h"
#include "engine/status.h"
#include "engine/superblock.h"

namespace engine {

// DiskManager is the bottom of the stack: fixed-size page read/write to a
// single file, nothing else. It knows the layout of exactly one page (the
// superblock at page 0); every other page is opaque bytes to it.
class DiskManager {
  public:
    enum class SyncPolicy {
        kEveryWrite, // fsync after every WritePage
        kNever,      // never fsync, it is useful for isolating the fsync cost
    };

    // On a fresh file, writes and fsyncs a new superblock before returning,
    // so a crash right after Open() still leaves a valid, re-openable file.
    // On an existing file, reads and validates the superblock, failing with
    // Corruption if the magic/version/size don't check out.
    static StatusOr<std::unique_ptr<DiskManager>>
    Open(const std::string& path,
         uint32_t page_size,
         bool create_if_missing,
         SyncPolicy sync_policy = SyncPolicy::kEveryWrite);

    ~DiskManager();
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // out_buf must point to at least page_size() writable bytes. Returns
    // Corruption on a short read rather than partially-read garbage.
    Status ReadPage(page_id_t page_id, char* out_buf);

    // in_buf must point to at least page_size() readable bytes.
    Status WritePage(page_id_t page_id, const char* in_buf);

    // Extends the file by one zero-filled page.
    StatusOr<page_id_t> AllocatePage();

    uint32_t GetNumPages() const {
        return superblock_.page_count;
    }
    uint32_t page_size() const {
        return superblock_.page_size;
    }

    page_id_t GetFreeListHead() const {
        return superblock_.free_list_head_page_id;
    }
    Status SetFreeListHead(page_id_t head) {
        superblock_.free_list_head_page_id = head;
        return WriteSuperblock();
    }

    page_id_t GetRootPageId() const {
        return superblock_.root_page_id;
    }
    Status SetRootPageId(page_id_t root) {
        superblock_.root_page_id = root;
        return WriteSuperblock();
    }

    Status Shutdown();

  private:
    DiskManager(int fd, Superblock superblock, SyncPolicy sync_policy);

    Status WriteSuperblock();
    Status SyncIfNeeded();

    // Wrappers around pread/pwrite that loop on short reads/writes
    Status PreadFull(void* buf, size_t count, off_t offset) const;
    Status PwriteFull(const void* buf, size_t count, off_t offset) const;

    int fd_;
    Superblock superblock_;
    SyncPolicy sync_policy_;
    bool shutdown_ = false;
};

} // namespace engine
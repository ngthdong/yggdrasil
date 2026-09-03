#pragma once

#include <memory>
#include <string>

#include "engine/b_plus_tree.h"
#include "engine/buffer_pool_manager.h"
#include "engine/disk_manager.h"
#include "engine/free_page_manager.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// Provides read-only access to a point-in-time database snapshot.
//
// A Snapshot owns an independent storage stack used to read the database
// state captured when the snapshot was created. It does not modify the
// underlying database.
//
// snapshot is valid while its internal storage resources are available.
// Move operations transfer ownership; copy operations are disabled.
class Snapshot {
  public:
    Snapshot() = default;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    ~Snapshot();

    StatusOr<std::string> Get(const Slice& key);
    bool is_valid() const {
        return tree_ != nullptr;
    }

  private:
    friend class Database;

    Snapshot(std::unique_ptr<DiskManager> disk_manager,
             std::unique_ptr<BufferPoolManager> bpm,
             std::unique_ptr<FreePageManager> fpm,
             std::unique_ptr<BPlusTree> tree,
             std::string snapshot_file_path);

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<FreePageManager> free_page_manager_;
    std::unique_ptr<BPlusTree> tree_;
    std::string snapshot_file_path_;
};

} // namespace engine
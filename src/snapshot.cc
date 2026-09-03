#include "engine/snapshot.h"
#include <cstdio>

namespace engine {

Snapshot::Snapshot(std::unique_ptr<DiskManager> disk_manager,
                   std::unique_ptr<BufferPoolManager> bpm,
                   std::unique_ptr<FreePageManager> fpm,
                   std::unique_ptr<BPlusTree> tree,
                   std::string snapshot_file_path)
    : disk_manager_(std::move(disk_manager)), buffer_pool_manager_(std::move(bpm)),
      free_page_manager_(std::move(fpm)), tree_(std::move(tree)),
      snapshot_file_path_(std::move(snapshot_file_path)) {}

Snapshot::~Snapshot() {
    tree_.reset();
    free_page_manager_.reset();
    buffer_pool_manager_.reset();
    disk_manager_.reset();
    if (!snapshot_file_path_.empty()) {
        std::remove(snapshot_file_path_.c_str());
    }
}

StatusOr<std::string> Snapshot::Get(const Slice& key) {
    if (!is_valid()) {
        return Status::InvalidArgument("Snapshot::Get: this snapshot is not valid");
    }
    return tree_->Get(key);
}

} // namespace engine
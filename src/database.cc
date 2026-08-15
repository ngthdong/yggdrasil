#include "engine/database.h"

namespace engine {

Database::Database(Options options) : options_(std::move(options)) {}

Database::~Database() {
    if (is_open_) {
        Close();
    }
}

Status Database::EnsureOpen() const {
    if (!is_open_) {
        return Status::InvalidArgument("Database: operation attempted while not open");
    }
    return Status::OK();
}

Status Database::Open() {
    if (is_open_) {
        return Status::InvalidArgument("Database is already open");
    }
    Status validate = options_.Validate();
    if (!validate.ok()) {
        return validate;
    }

    StatusOr<std::unique_ptr<DiskManager>> dm_or =
        DiskManager::Open(options_.path, options_.page_size, options_.create_if_missing);
    if (!dm_or.ok()) {
        return dm_or.status();
    }
    disk_manager_ = std::move(dm_or.value());

    buffer_pool_manager_ =
        std::make_unique<BufferPoolManager>(disk_manager_.get(), options_.buffer_pool_frames);
    free_page_manager_ =
        std::make_unique<FreePageManager>(disk_manager_.get(), buffer_pool_manager_.get());
    tree_ = std::make_unique<BPlusTree>(
        disk_manager_.get(), buffer_pool_manager_.get(), free_page_manager_.get());

    is_open_ = true;
    return Status::OK();
}

Status Database::Close() {
    if (!is_open_) {
        return Status::OK();
    }

    Status flush_s = buffer_pool_manager_->FlushAllPages();
    Status shutdown_s = disk_manager_->Shutdown();

    tree_.reset();
    free_page_manager_.reset();
    buffer_pool_manager_.reset();
    disk_manager_.reset();
    is_open_ = false;

    if (!flush_s.ok()) {
        return flush_s;
    }
    return shutdown_s;
}

Status Database::Put(const Slice& key, const Slice& value) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    Status insert_s = tree_->Insert(key, value);
    if (insert_s.ok()) {
        return Status::OK();
    }
    if (insert_s.code() != Status::Code::kInvalidArgument) {
        return insert_s;
    }

    Status remove_s = tree_->Remove(key);
    if (!remove_s.ok()) {
        return remove_s;
    }
    return tree_->Insert(key, value);
}

StatusOr<std::string> Database::Get(const Slice& key) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    return tree_->Get(key);
}

Status Database::Remove(const Slice& key) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    return tree_->Remove(key);
}

StatusOr<Database::Iterator> Database::NewIterator() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    StatusOr<BPlusTreeIterator> it_or = tree_->Begin();
    if (!it_or.ok()) {
        return it_or.status();
    }
    return Iterator(std::move(it_or.value()));
}

StatusOr<Database::Iterator> Database::NewIterator(const Slice& start_key) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    StatusOr<BPlusTreeIterator> it_or = tree_->Begin(start_key);
    if (!it_or.ok()) {
        return it_or.status();
    }
    return Iterator(std::move(it_or.value()));
}

StatusOr<DBStats> Database::GetStats() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    StatusOr<int> height_or = tree_->Height();
    if (!height_or.ok()) {
        return height_or.status();
    }

    DBStats stats;
    stats.page_count = disk_manager_->GetNumPages();
    stats.buffer_pool_capacity_frames = buffer_pool_manager_->CapacityFrames();
    stats.buffer_pool_resident_frames =
        buffer_pool_manager_->CapacityFrames() - buffer_pool_manager_->FreeFrameCount();
    stats.buffer_pool_hits = buffer_pool_manager_->HitCount();
    stats.buffer_pool_misses = buffer_pool_manager_->MissCount();
    stats.tree_height = height_or.value();
    return stats;
}

Status Database::Verify() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    return tree_->Verify();
}

} // namespace engine
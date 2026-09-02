#include "engine/database.h"
#include <cstdio>

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

    std::string wal_path = options_.path + ".wal";

    last_open_ran_recovery_ = false;
    {
        FILE* probe = std::fopen(wal_path.c_str(), "rb");
        if (probe != nullptr) {
            std::fseek(probe, 0, SEEK_END);
            long size = std::ftell(probe);
            std::fclose(probe);
            last_open_ran_recovery_ = (size > 0);
        }
    }
    RecoveryManager recovery(buffer_pool_manager_.get(), tree_.get(), wal_path);
    Status recover_s = recovery.Recover();
    if (!recover_s.ok()) {
        return recover_s;
    }

    StatusOr<std::unique_ptr<WalManager>> wal_or = WalManager::Open(wal_path);
    if (!wal_or.ok()) {
        return wal_or.status();
    }
    wal_manager_ = std::move(wal_or.value());
    buffer_pool_manager_->SetWalManager(wal_manager_.get());

    checkpoint_manager_ = std::make_unique<CheckpointManager>(
        disk_manager_.get(), buffer_pool_manager_.get(), wal_manager_.get());
    lock_manager_ = std::make_unique<LockManager>(options_.deadlock_policy);
    if (options_.deadlock_policy == DeadlockPolicy::kDetection) {
        deadlock_detector_ = std::make_unique<DeadlockDetector>(
            lock_manager_.get(), options_.deadlock_detection_interval);
        deadlock_detector_->Start();
    }

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
    checkpoint_manager_.reset();
    deadlock_detector_.reset();
    lock_manager_.reset();
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

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

    Status txn_check = EnsureNoActiveTransaction();
    if (!txn_check.ok()) {
        return txn_check;
    }

    StatusOr<lsn_t> lsn_or =
        wal_manager_->AppendLogRecord(LogRecordType::kInsert, kInvalidPageId, key, value);
    if (!lsn_or.ok()) {
        return lsn_or.status();
    }
    lsn_t lsn = lsn_or.value();

    Status insert_s = tree_->Insert(key, value, lsn);
    if (!insert_s.ok()) {
        if (insert_s.code() != Status::Code::kInvalidArgument) {
            return insert_s;
        }

        Status remove_s = tree_->Remove(key, lsn);
        if (!remove_s.ok()) {
            return remove_s;
        }
        insert_s = tree_->Insert(key, value, lsn);
        if (!insert_s.ok()) {
            return insert_s;
        }
    }

    if (options_.sync_on_commit) {
        Status flush_s = wal_manager_->Flush(lsn);
        if (!flush_s.ok()) {
            return flush_s;
        }
    }
    return Status::OK();
}

StatusOr<std::string> Database::Get(const Slice& key) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    std::lock_guard<std::mutex> engine_lock(engine_mutex_);
    return tree_->Get(key);
}

Status Database::Remove(const Slice& key) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

    Status txn_check = EnsureNoActiveTransaction();
    if (!txn_check.ok()) {
        return txn_check;
    }

    StatusOr<std::string> old_value_or = tree_->Get(key);
    if (!old_value_or.ok()) {
        return old_value_or.status();
    }

    StatusOr<lsn_t> lsn_or = wal_manager_->AppendLogRecord(
        LogRecordType::kDelete, kInvalidPageId, key, Slice(old_value_or.value()));
    if (!lsn_or.ok()) {
        return lsn_or.status();
    }
    lsn_t lsn = lsn_or.value();

    Status s = tree_->Remove(key, lsn);
    if (!s.ok()) {
        return s;
    }
    if (options_.sync_on_commit) {
        Status flush_s = wal_manager_->Flush(lsn);
        if (!flush_s.ok()) {
            return flush_s;
        }
    }
    return Status::OK();
}

StatusOr<Database::Iterator> Database::NewIterator() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

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

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

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

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

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
    std::lock_guard<std::mutex> engine_lock(engine_mutex_);
    return tree_->Verify();
}

Status Database::Checkpoint() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

    Status txn_check = EnsureNoActiveTransaction();
    if (!txn_check.ok()) {
        return txn_check;
    }

    return checkpoint_manager_->TakeCheckpoint();
}

Status Database::EnsureNoActiveTransaction() const {
    if (active_txn_count_ > 0) {
        return Status::InvalidArgument("Database: a Transaction is active, using it directly, or "
                                       "Commit()/Rollback() it first");
    }
    return Status::OK();
}

void Database::OnTransactionFinalized(txn_id_t /*txn_id*/) {
    std::lock_guard<std::mutex> engine_lock(engine_mutex_);
    if (active_txn_count_ > 0) {
        active_txn_count_--;
    }
}

StatusOr<Transaction> Database::BeginTransaction() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    txn_id_t txn_id;
    {
        std::lock_guard<std::mutex> engine_lock(engine_mutex_);
        txn_id = next_txn_id_++;
        active_txn_count_++;
    }
    StatusOr<lsn_t> lsn_or = wal_manager_->AppendLogRecord(
        LogRecordType::kBegin, kInvalidPageId, Slice(""), Slice(""), txn_id);
    if (!lsn_or.ok()) {
        std::lock_guard<std::mutex> engine_lock(engine_mutex_);
        active_txn_count_--;
        return lsn_or.status();
    }
    return Transaction(this,
                       tree_.get(),
                       wal_manager_.get(),
                       lock_manager_.get(),
                       &engine_mutex_,
                       txn_id,
                       options_.sync_on_commit);
}

} // namespace engine
#include "engine/database.h"
#include "engine/file_utils.h"

#include <cstdio>
#include <set>

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

Status Database::Write(const WriteBatch& batch) {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }
    if (batch.empty()) {
        return Status::OK();
    }

    txn_id_t txn_id;
    {
        std::lock_guard<std::mutex> engine_lock(engine_mutex_);
        Status txn_check = EnsureNoActiveTransaction();
        if (!txn_check.ok()) {
            return txn_check;
        }
        txn_id = next_txn_id_++;
    }

    // Sorted-order lock acquisition. This must happen without holding
    // engine_mutex_: AcquireLock can block waiting on another transaction,
    // and under wound-wait that transaction's rollback needs engine_mutex_
    // to apply its logical undo before it can release the lock we're
    // waiting on. Holding engine_mutex_ across this call would deadlock
    // against that rollback.
    std::set<std::string> unique_keys;
    for (const auto& op : batch.ops_) {
        unique_keys.insert(op.key);
    }

    std::vector<std::string> acquired_keys;
    Status lock_s = Status::OK();
    for (const auto& key : unique_keys) {
        lock_s = lock_manager_->AcquireLock(txn_id, key, LockMode::kExclusive);
        if (!lock_s.ok()) {
            break;
        }
        acquired_keys.push_back(key);
    }
    if (!lock_s.ok()) {
        lock_manager_->ReleaseAllLocks(txn_id);
        return lock_s;
    }

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

    // Without a Begin record, RecoveryManager::Analysis() never starts
    // tracking this txn_id, so a crash between here and the Commit record
    // below would leave Redo() replaying the partially-applied ops with no
    // matching Undo() entry to roll them back.
    StatusOr<lsn_t> begin_lsn_or = wal_manager_->AppendLogRecord(
        LogRecordType::kBegin, kInvalidPageId, Slice(""), Slice(""), txn_id);
    if (!begin_lsn_or.ok()) {
        lock_manager_->ReleaseAllLocks(txn_id);
        return begin_lsn_or.status();
    }

    // Apply original submisstion order.
    std::vector<LogRecord> applied;
    Status apply_s = Status::OK();
    for (const auto& op : batch.ops_) {
        if (op.type == LogRecordType::kInsert) {
            // BPlusTree::Insert() rejects duplicate keys, so a batch
            // touching the same key twice would fail on the second op
            // without this upsert composition, mirroring Database::Put's
            // own logic.
            StatusOr<std::string> existing_or = tree_->Get(Slice(op.key));
            if (existing_or.ok()) {
                StatusOr<lsn_t> del_lsn_or =
                    wal_manager_->AppendLogRecord(LogRecordType::kDelete,
                                                  kInvalidPageId,
                                                  Slice(op.key),
                                                  Slice(existing_or.value()),
                                                  txn_id);
                if (!del_lsn_or.ok()) {
                    apply_s = del_lsn_or.status();
                    break;
                }
                lsn_t del_lsn = del_lsn_or.value();
                apply_s = tree_->Remove(Slice(op.key), del_lsn);
                if (!apply_s.ok()) {
                    break;
                }

                LogRecord del_rec;
                del_rec.lsn = del_lsn;
                del_rec.txn_id = txn_id;
                del_rec.type = LogRecordType::kDelete;
                del_rec.key = op.key;
                del_rec.value = existing_or.value();
                applied.push_back(std::move(del_rec));
            }
            StatusOr<lsn_t> lsn_or = wal_manager_->AppendLogRecord(
                LogRecordType::kInsert, kInvalidPageId, Slice(op.key), Slice(op.value), txn_id);
            if (!lsn_or.ok()) {
                apply_s = lsn_or.status();
                break;
            }
            lsn_t lsn = lsn_or.value();
            apply_s = tree_->Insert(Slice(op.key), Slice(op.value), lsn);
            if (!apply_s.ok()) {
                break;
            }

            LogRecord rec;
            rec.lsn = lsn;
            rec.txn_id = txn_id;
            rec.type = LogRecordType::kInsert;
            rec.key = op.key;
            rec.value = op.value;
            applied.push_back(std::move(rec));

        } else { // kDelete
            StatusOr<std::string> old_or = tree_->Get(Slice(op.key));
            if (!old_or.ok()) {
                continue;
            }
            StatusOr<lsn_t> lsn_or = wal_manager_->AppendLogRecord(LogRecordType::kDelete,
                                                                   kInvalidPageId,
                                                                   Slice(op.key),
                                                                   Slice(old_or.value()),
                                                                   txn_id);
            if (!lsn_or.ok()) {
                apply_s = lsn_or.status();
                break;
            }
            lsn_t lsn = lsn_or.value();
            apply_s = tree_->Remove(Slice(op.key), lsn);
            if (!apply_s.ok()) {
                break;
            }

            LogRecord rec;
            rec.lsn = lsn;
            rec.txn_id = txn_id;
            rec.type = LogRecordType::kDelete;
            rec.key = op.key;
            rec.value = old_or.value();
            applied.push_back(std::move(rec));
        }
    }

    if (!apply_s.ok()) {
        ApplyLogicalUndo(tree_.get(), applied);
        lock_manager_->ReleaseAllLocks(txn_id);
        return apply_s;
    }

    StatusOr<lsn_t> commit_lsn_or = wal_manager_->AppendLogRecord(
        LogRecordType::kCommit, kInvalidPageId, Slice(""), Slice(""), txn_id);
    if (!commit_lsn_or.ok()) {
        ApplyLogicalUndo(tree_.get(), applied);
        lock_manager_->ReleaseAllLocks(txn_id);
        return commit_lsn_or.status();
    }

    if (options_.sync_on_commit) {
        Status flush_s = wal_manager_->Flush(commit_lsn_or.value());
        if (!flush_s.ok()) {
            lock_manager_->ReleaseAllLocks(txn_id);
            return flush_s;
        }
    }

    lock_manager_->ReleaseAllLocks(txn_id);
    return Status::OK();
}

StatusOr<Snapshot> Database::CreateSnapshot() {
    Status open_check = EnsureOpen();
    if (!open_check.ok()) {
        return open_check;
    }

    std::lock_guard<std::mutex> engine_lock(engine_mutex_);

    // Persist all dirty pages before copying the database state so
    // the snapshot is created from a consistent on-disk representation.
    Status flush_s = buffer_pool_manager_->FlushAllPages();
    if (!flush_s.ok()) {
        return flush_s;
    }

    std::string snapshot_path = options_.path + ".snapshot." + std::to_string(next_snapshot_id_++);
    Status copy_s = CopyFile(options_.path, snapshot_path); // raw POSIX read/write, 1 MiB chunks
    if (!copy_s.ok()) {
        return copy_s;
    }

    StatusOr<std::unique_ptr<DiskManager>> dm_or =
        DiskManager::Open(snapshot_path, options_.page_size, /*create_if_missing=*/false);
    if (!dm_or.ok()) {
        std::remove(snapshot_path.c_str());
        return dm_or.status();
    }
    auto snap_dm = std::move(dm_or.value());
    auto snap_bpm = std::make_unique<BufferPoolManager>(snap_dm.get(), options_.buffer_pool_frames);
    auto snap_fpm = std::make_unique<FreePageManager>(snap_dm.get(), snap_bpm.get());
    auto snap_tree = std::make_unique<BPlusTree>(snap_dm.get(), snap_bpm.get(), snap_fpm.get());

    return Snapshot(std::move(snap_dm),
                    std::move(snap_bpm),
                    std::move(snap_fpm),
                    std::move(snap_tree),
                    snapshot_path);
}

} // namespace engine
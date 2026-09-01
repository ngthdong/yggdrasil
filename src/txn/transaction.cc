#include "engine/transaction.h"
#include "engine/database.h"
#include "engine/undo_utils.h"

namespace engine {

Transaction::Transaction(
    Database* db, BPlusTree* tree, WalManager* wal, txn_id_t txn_id, bool sync_on_commit)
    : db_(db), tree_(tree), wal_(wal), txn_id_(txn_id), sync_on_commit_(sync_on_commit) {}

Transaction::Transaction(Transaction&& other) noexcept {
    *this = std::move(other);
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        if (is_active()) {
            Rollback(); // do not silently leak an unresolved transaction being overwrite
        }
        db_ = other.db_;
        tree_ = other.tree_;
        wal_ = other.wal_;
        txn_id_ = other.txn_id_;
        sync_on_commit_ = other.sync_on_commit_;
        finalized_ = other.finalized_;
        has_error_ = other.has_error_;
        moved_from_ = other.moved_from_;
        records_ = std::move(other.records_);
        other.moved_from_ = true;
    }
    return *this;
}

Transaction::~Transaction() {
    if (is_active()) {
        Rollback(); // best-effort
    }
}

Status Transaction::LogAndApply(LogRecordType type, const Slice& key, const Slice& value) {
    StatusOr<lsn_t> lsn_or = wal_->AppendLogRecord(type, kInvalidPageId, key, value, txn_id_);
    if (!lsn_or.ok()) {
        has_error_ = true;
        return lsn_or.status();
    }
    lsn_t lsn = lsn_or.value();

    LogRecord record;
    record.lsn = lsn;
    record.txn_id = txn_id_;
    record.type = type;
    record.key = key.ToString();
    record.value = value.ToString();

    Status s =
        (type == LogRecordType::kInsert) ? tree_->Insert(key, value, lsn) : tree_->Remove(key, lsn);
    if (!s.ok()) {
        has_error_ = true;
        return s;
    }
    records_.push_back(std::move(record));
    return Status::OK();
}

Status Transaction::Put(const Slice& key, const Slice& value) {
    if (!is_active()) {
        return Status::InvalidArgument("Transaction::Put: transaction is not active.");
    }
    StatusOr<std::string> old_value_or = tree_->Get(Slice(key));
    if (old_value_or.ok()) {
        Status del_s = LogAndApply(LogRecordType::kDelete, key, Slice(old_value_or.value()));
        if (!del_s.ok()) {
        }
    }
    return LogAndApply(LogRecordType::kInsert, key, value);
}

Status Transaction::Remove(const Slice& key) {
    if (!is_active()) {
        return Status::InvalidArgument("Transaction::Remove: transaction is not active.");
    }
    StatusOr<std::string> old_value_or = tree_->Get(Slice(key));
    if (!old_value_or.ok()) {
        return old_value_or.status();
    }
    return LogAndApply(LogRecordType::kDelete, key, Slice(old_value_or.value()));
}

Status Transaction::Commit() {
    if (finalized_) {
        return Status::InvalidArgument("Transaction::Commit: already finalized.");
    }
    if (has_error_) {
        return Status::InvalidArgument("Transaction::Commit: this transaction hit a error earlier "
                                       "and can only be rolled back.");
    }
    StatusOr<lsn_t> lsn_or = wal_->AppendLogRecord(
        LogRecordType::kCommit, kInvalidPageId, Slice(""), Slice(""), txn_id_);
    if (!lsn_or.ok()) {
        return lsn_or.status();
    }
    lsn_t lsn = lsn_or.value();

    if (sync_on_commit_) {
        Status flush_s = wal_->Flush(lsn);
        if (!flush_s.ok()) {
            return flush_s;
        }
    }

    finalized_ = true;
    db_->OnTransactionFinalized(txn_id_);
    return Status::OK();
}

Status Transaction::Rollback() {
    if (finalized_) {
        return Status::InvalidArgument("Transaction::Roolback: already finalized.");
    }
    Status undo_s = ApplyLogicalUndo(tree_, records_);
    if (!undo_s.ok()) {
        return undo_s;
    }

    StatusOr<lsn_t> lsn_or =
        wal_->AppendLogRecord(LogRecordType::kAbort, kInvalidPageId, Slice(""), Slice(""), txn_id_);
    if (!lsn_or.ok()) {
        return lsn_or.status();
    }
    if (sync_on_commit_) {
        Status flush_s = wal_->Flush(lsn_or.value());
        if (!flush_s.ok()) {
            return flush_s;
        }
    }

    finalized_ = true;
    db_->OnTransactionFinalized(txn_id_);
    return Status::OK();
}

} // namespace engine
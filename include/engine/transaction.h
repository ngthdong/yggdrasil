#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "engine/b_plus_tree.h"
#include "engine/lock_manager.h"
#include "engine/log_record.h"
#include "engine/slice.h"
#include "engine/status.h"
#include "engine/wal_manager.h"

namespace engine {

class Database;

// Represents an atomic unit of work.
//
// Transactions provide atomicity but not isolation. Writes are applied
// directly to the shared BPlusTree and are visible to regular reads before
// commit.
//
// An unfinalized transaction is automatically rolled back on destruction.
class Transaction {
  public:
    Transaction() = default;
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    StatusOr<std::string> Get(const Slice& key);

    // Inserts or updates a key within the transaction.
    // For an existing key, the previous value is preserved in the transaction's
    // undo records so that Rollback() restores the original state.
    Status Put(const Slice& key, const Slice& value);

    // Removes a key from the transaction.
    // Returns NotFound if the key does not exist. A failed removal does not
    // modify the transaction's state.
    Status Remove(const Slice& key);

    // Commits all changes made by the transaction.
    // A transaction with a previous Put() or Remove() failure cannot be
    // committed and must be rolled back instead.
    Status Commit();

    // Rolls back all changes made by the transaction.
    // Operations are undone in reverse order using the same logical undo rules
    // used by crash recovery. The transaction is finalized only after the
    // rollback completes successfully.
    Status Rollback();

    // Returns true if the transaction owns valid resources and has not been
    // finalized or moved from.
    bool is_active() const {
        return !finalized_ && !moved_from_ && (txn_id_ != kInvalidTxnId);
    }

    txn_id_t ixn_id() {
        return txn_id_;
    }

  private:
    friend class Database;
    Transaction(Database* db,
                BPlusTree* tree,
                WalManager* wal,
                LockManager* lock_manager,
                std::mutex* engine_mutex,
                txn_id_t txn_id,
                bool sync_on_commit);
    Status LogAndApply(LogRecordType type, const Slice& key, const Slice& value);
    Status AcquireAndCheckWound(const std::string& key, LockMode mode);
    Status SelfRollbackAfterWound();

    Database* db_ = nullptr;
    BPlusTree* tree_ = nullptr;
    WalManager* wal_ = nullptr;
    LockManager* lock_manager_ = nullptr;
    std::mutex* engine_mutex_ = nullptr;
    txn_id_t txn_id_ = kInvalidTxnId;
    bool sync_on_commit_ = true;
    bool finalized_ = false;
    bool has_error_ = false;
    bool moved_from_ = false;
    std::vector<LogRecord> records_;
};

} // namespace engine
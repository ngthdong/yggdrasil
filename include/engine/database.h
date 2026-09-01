#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "engine/b_plus_tree.h"
#include "engine/bptree_iterator.h"
#include "engine/buffer_pool_manager.h"
#include "engine/checkpoint_manager.h"
#include "engine/disk_manager.h"
#include "engine/free_page_manager.h"
#include "engine/lock_manager.h"
#include "engine/options.h"
#include "engine/recovery_manager.h"
#include "engine/slice.h"
#include "engine/stats.h"
#include "engine/status.h"
#include "engine/transaction.h"
#include "engine/wal_manager.h"

namespace engine {

class Database {
  public:
    class Iterator {
      public:
        Iterator() = default;
        Iterator(Iterator&&) = default;
        Iterator& operator=(Iterator&&) = default;
        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        bool Valid() const {
            return inner_.Valid();
        }
        Slice Key() {
            return inner_.Key();
        }
        Slice Value() {
            return inner_.Value();
        }
        Status Next() {
            return inner_.Next();
        }

      private:
        friend class Database;
        explicit Iterator(BPlusTreeIterator inner) : inner_(std::move(inner)) {}
        BPlusTreeIterator inner_;
    };

    explicit Database(Options options);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status Open();
    Status Close();

    bool is_open() const {
        return is_open_;
    }
    const Options& options() const {
        return options_;
    }
    bool last_open_ran_recovery() const {
        return last_open_ran_recovery_;
    }

    Status Put(const Slice& key, const Slice& value);

    StatusOr<std::string> Get(const Slice& key);
    Status Remove(const Slice& key);

    StatusOr<Iterator> NewIterator();
    StatusOr<Iterator> NewIterator(const Slice& start_key);

    StatusOr<DBStats> GetStats();
    Status Verify();

    Status Checkpoint();

    StatusOr<Transaction> BeginTransaction();
    bool has_active_transaction() const {
        return active_txn_count_ > 0;
    }

  private:
    friend class Transaction;

    Status EnsureNoActiveTransaction() const;
    void OnTransactionFinalized(txn_id_t txn_id);
    txn_id_t next_txn_id_ = 1;
    txn_id_t active_txn_count_ = 0;

    Status EnsureOpen() const;

    Options options_;
    bool is_open_ = false;
    bool last_open_ran_recovery_ = false;

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<FreePageManager> free_page_manager_;
    std::unique_ptr<BPlusTree> tree_;
    std::unique_ptr<WalManager> wal_manager_;
    std::unique_ptr<CheckpointManager> checkpoint_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::mutex engine_mutex_;
};

} // namespace engine
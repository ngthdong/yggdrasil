#pragma once
#include "engine/b_plus_tree.h"
#include "engine/buffer_pool_manager.h"
#include "engine/log_record.h"
#include "engine/status.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class RecoveryManager {
  public:
    RecoveryManager(BufferPoolManager* buffer_pool_manager, BPlusTree* tree, std::string wal_path)
        : buffer_pool_manager_(buffer_pool_manager), tree_(tree), wal_path_(wal_path) {}

    // Reads and parses the WAL file at wal_path, then runs Analysis, Redo,
    // Undo, and a final FlushAllPages so recovered state is durable before
    // returning. MUST be called before a fresh WalManager is opened against
    // the same path.
    Status Recover();

  private:
    struct TxnInfo {
        std::vector<LogRecord> records; // this txn's own kInsert/kDelete records
        bool committed = false;
    };

    StatusOr<std::vector<LogRecord>> ParseAllRecords();
    static void Analysis(const std::vector<LogRecord>& records,
                         std::unordered_map<txn_id_t, TxnInfo>* txns);
    Status Redo(const std::vector<LogRecord>& records);
    Status Undo(const std::unordered_map<txn_id_t, TxnInfo>& txns);

    BufferPoolManager* buffer_pool_manager_;
    BPlusTree* tree_;
    std::string wal_path_;
};

} // namespace engine
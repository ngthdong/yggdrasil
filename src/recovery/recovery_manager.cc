#include "engine/recovery_manager.h"
#include "cstdio"

#include <ranges>

namespace engine {

StatusOr<std::vector<LogRecord>> RecoveryManager::ParseAllRecords() {
    std::vector<LogRecord> records;
    FILE* f = std::fopen(wal_path_.c_str(), "rb");
    if (f == nullptr) {
        return records;
    }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return records;
    }

    std::vector<char> buf(static_cast<size_t>(size));
    size_t nread = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (nread != buf.size()) {
        return Status::IOError("RecoveryManager: short read of WAL file");
    }

    size_t offset = 0;
    while (offset < buf.size()) {
        size_t consumed = 0;
        StatusOr<LogRecord> rec_or =
            LogRecord::ParseFrom(buf.data(), buf.size(), offset, &consumed);
        if (!rec_or.ok()) {
            break;
        }

        records.push_back(rec_or.value());
        offset += consumed;
    }

    return records;
}

void RecoveryManager::Analysis(const std::vector<LogRecord>& records,
                               std::unordered_map<txn_id_t, TxnInfo>* txns) {
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::kBegin) {
            (*txns)[rec.txn_id] = TxnInfo{};
        } else if (rec.type == LogRecordType::kInsert || rec.type == LogRecordType::kDelete) {
            if (rec.txn_id != kInvalidTxnId) {
                auto it = txns->find(rec.txn_id);
                if (it != txns->end()) {
                    it->second.records.push_back(rec);
                }
            }
        } else if (rec.type == LogRecordType::kCommit) {
            auto it = txns->find(rec.txn_id);
            if (it != txns->end()) {
                it->second.committed = true;
            }
        }
    }
}

Status RecoveryManager::Redo(const std::vector<LogRecord>& records) {
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::kInsert) {
            StatusOr<std::string> v = tree_->Get(Slice(rec.key));
            if (v.ok() && v.value() == rec.value) {
                continue;
            }
            if (v.ok()) {
                Status rs = tree_->Remove(Slice(rec.key));
                if (!rs.ok()) {
                    return rs;
                }
            }

            Status is = tree_->Insert(Slice(rec.key), Slice(rec.value));
            if (!is.ok()) {
                return is;
            }
        } else if (rec.type == LogRecordType::kDelete) {
            StatusOr<std::string> v = tree_->Get(Slice(rec.key));
            if (!v.ok()) {
                continue; // already adsent
            }

            Status rs = tree_->Remove(Slice(rec.key));
            if (!rs.ok()) {
                return rs;
            }
        }
    }

    return Status::OK();
}

Status RecoveryManager::Undo(const std::unordered_map<txn_id_t, TxnInfo>& txns) {
    for (const auto& [txn_id, info] : txns) {
        if (info.committed) {
            continue;
        }
        for (const auto& rec : info.records | std::views::reverse) {
            if (rec.type == LogRecordType::kInsert) {
                StatusOr<std::string> v = tree_->Get(Slice(rec.key));
                if (v.ok() && v.value() == rec.value) {
                    Status s = tree_->Remove(Slice(rec.key));
                    if (!s.ok()) {
                        return s;
                    }
                }
            } else if (rec.type == LogRecordType::kDelete) {
                StatusOr<std::string> v = tree_->Get(Slice(rec.key));
                if (!v.ok()) {
                    Status s = tree_->Insert(Slice(rec.key), Slice(rec.value));
                    if (!s.ok()) {
                        return s;
                    }
                }
            }
        }
    }
    return Status::OK();
}

Status RecoveryManager::Recover() {
    StatusOr<std::vector<LogRecord>> records_or = ParseAllRecords();
    if (!records_or.ok()) {
        return records_or.status();
    }

    std::vector<LogRecord> records = std::move(records_or.value());
    if (records.empty()) {
        return Status::OK();
    }

    std::unordered_map<txn_id_t, TxnInfo> txns;
    Analysis(records, &txns);

    Status redo_s = Redo(records);
    if (!redo_s.ok()) {
        return redo_s;
    }

    Status undo_s = Undo(txns);
    if (!undo_s.ok()) {
        return undo_s;
    }

    // Recovered state becomes the new ground truth, making it durable
    // immediately rather than leaving it dirty-in-memory where a second
    // crash (before anything else happens) could lose it all over again.
    return buffer_pool_manager_->FlushAllPages();
}

} // namespace engine
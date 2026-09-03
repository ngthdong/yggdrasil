#pragma once

#include <string>
#include <vector>

#include "engine/log_record.h"
#include "engine/slice.h"

namespace engine {

// Collects a sequence of write operations for atomic application.
//
// WriteBatch stores Put and Delete operations in insertion order. The batch
// itself does not modify database state; Database applies all operations as
// a single transaction.
//
// Operations are owned by the batch and remain valid until the batch is
// cleared or destroyed.
class WriteBatch {
  public:
    void Put(const Slice& key, const Slice& value) {
        ops_.push_back(Op{
            LogRecordType::kInsert,
            key.ToString(),
            value.ToString(),
        });
    }

    void Delete(const Slice& key) {
        ops_.push_back(Op{LogRecordType::kDelete, key.ToString(), std::string()});
    }

    void Clear() {
        ops_.clear();
    }

    size_t size() const {
        return ops_.size();
    }

    bool empty() const {
        return ops_.empty();
    }

  private:
    friend class Database;

    struct Op {
        LogRecordType type;
        std::string key;
        std::string value;
    };

    std::vector<Op> ops_;
};

} // namespace engine
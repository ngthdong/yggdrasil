#include "engine/undo_utils.h"

#include <ranges>

namespace engine {

Status ApplyLogicalUndo(BPlusTree* tree, const std::vector<LogRecord>& records) {
    for (const auto& rec : records | std::views::reverse) {
        if (rec.type == LogRecordType::kInsert) {
            StatusOr<std::string> v = tree->Get(Slice(rec.key));
            if (v.ok() && v.value() == rec.value) {
                Status s = tree->Remove(Slice(rec.key));
                if (!s.ok()) {
                    return s;
                }
            }
        } else if (rec.type == LogRecordType::kDelete) {
            StatusOr<std::string> v = tree->Get(Slice(rec.key));
            if (!v.ok()) {
                Status s = tree->Insert(Slice(rec.key), Slice(rec.value));
                if (!s.ok()) {
                    return s;
                }
            }
        }
    }
    return Status::OK();
}

} // namespace engine
#include "engine/bptree_iterator.h"

#include "engine/bptree_leaf_page.h"

namespace engine {

Slice BPlusTreeIterator::Key() {
    BPlusTreeLeafPage leaf(guard_.mutable_data(), bpm_->page_size());
    return leaf.KeyAt(slot_index_);
}

Slice BPlusTreeIterator::Value() {
    BPlusTreeLeafPage leaf(guard_.mutable_data(), bpm_->page_size());
    return leaf.ValueAt(slot_index_);
}

Status BPlusTreeIterator::Next() {
    if (!Valid()) {
        return Status::InvalidArgument("BPlusTreeIterator::Next: called on an invalid iterator");
    }
    slot_index_ = static_cast<uint16_t>(slot_index_ + 1);
    return AdvancePastEndIfNeeded();
}

Status BPlusTreeIterator::AdvancePastEndIfNeeded() {
    // Defensive bound against a corrupted/cyclic next_leaf_page_id chain
    constexpr int kMaxHops = 1'000'000;
    int hops = 0;

    while (guard_.is_valid()) {
        BPlusTreeLeafPage leaf(guard_.mutable_data(), bpm_->page_size());
        if (slot_index_ < leaf.num_keys()) {
            return Status::OK();
        }

        page_id_t next_id = leaf.next_leaf_page_id();
        guard_.Reset(); // unpin the exhausted leaf before touching the next one
        if (next_id == kInvalidPageId) {
            return Status::OK();
        }

        if (++hops > kMaxHops) {
            return Status::Corruption(
                "BPlusTreeIterator: exceeded max leaf hops. Likely a corrupted "
                "or cyclic next_leaf_page_id chain");
        }

        StatusOr<PageGuard> next_guard_or = FetchPageGuarded(bpm_, next_id);
        if (!next_guard_or.ok()) {
            return next_guard_or.status();
        }
        guard_ = std::move(next_guard_or.value());
        slot_index_ = 0;
    }
    return Status::OK();
}

} // namespace engine
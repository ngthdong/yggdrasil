#include "engine/b_plus_tree.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace engine {

namespace {
PageType PeekPageType(const char* buf) {
    return static_cast<PageType>(static_cast<uint8_t>(buf[4]));
}
} // namespace

StatusOr<page_id_t> BPlusTree::GetOrCreateRootLeaf() {
    page_id_t root = disk_manager_->GetRootPageId();
    if (root != kInvalidPageId) {
        return root;
    }

    page_id_t new_id;
    StatusOr<char*> data_or = free_page_manager_->AllocateAndPinPage(&new_id);
    if (!data_or.ok()) {
        return data_or.status();
    }

    BPlusTreeLeafPage::InitNewPage(data_or.value(), buffer_pool_manager_->page_size(), new_id);

    Status unpin_s = buffer_pool_manager_->UnpinPage(new_id, /*is_dirty=*/true);
    if (!unpin_s.ok()) {
        return unpin_s;
    }

    Status set_s = disk_manager_->SetRootPageId(new_id);
    if (!set_s.ok()) {
        return set_s;
    }

    return new_id;
}

StatusOr<std::string> BPlusTree::Get(const Slice& key) {
    if (IsEmpty()) {
        return Status::NotFound("BPlusTree::Get: tree is empty");
    }

    page_id_t current = disk_manager_->GetRootPageId();
    while (true) {
        StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, current);
        if (!guard_or.ok()) {
            return guard_or.status();
        }
        PageGuard guard = std::move(guard_or.value());

        if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
            BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
            auto result = leaf.FindKey(key, comparator_);
            if (!result.found) {
                return Status::NotFound("BPlusTree::Get: key not found: " + key.ToString());
            }
            return leaf.ValueAt(result.index).ToString(); // copy while guard alive
        }

        BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
        uint16_t idx = internal.FindChildIndex(key, comparator_);
        current = internal.ChildAt(idx);
    }
}

Status BPlusTree::Insert(const Slice& key, const Slice& value) {
    StatusOr<page_id_t> root_or = GetOrCreateRootLeaf();
    if (!root_or.ok()) {
        return root_or.status();
    }

    StatusOr<InsertResult> result_or = InsertRecursive(root_or.value(), key, value);
    if (!result_or.ok()) {
        return result_or.status();
    }

    InsertResult result = result_or.value();
    if (!result.split_occurred) {
        return Status::OK();
    }

    page_id_t new_root_id;
    StatusOr<char*> data_or = free_page_manager_->AllocateAndPinPage(&new_root_id);
    if (!data_or.ok()) {
        return data_or.status();
    }
    PageGuard new_root_guard(buffer_pool_manager_, new_root_id, data_or.value());

    BPlusTreeInternalPage::InitNewPage(new_root_guard.mutable_data(),
                                       buffer_pool_manager_->page_size(),
                                       new_root_id,
                                       /*leftmost_child_id=*/root_or.value());
    BPlusTreeInternalPage new_root(new_root_guard.mutable_data(),
                                   buffer_pool_manager_->page_size());
    Status s = new_root.InsertEntry(0, Slice(result.split_key), result.new_right_child_id);
    if (!s.ok()) {
        return s; // should be unreachable: a fresh page always has room for one entry
    }
    new_root_guard.MarkDirty();

    return disk_manager_->SetRootPageId(new_root_id);
}

StatusOr<page_id_t> BPlusTree::DescendToLeaf(const Slice* key) {
    page_id_t current = disk_manager_->GetRootPageId();
    while (true) {
        StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, current);
        if (!guard_or.ok()) {
            return guard_or.status();
        }
        PageGuard guard = std::move(guard_or.value());

        if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
            return current;
        }

        BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
        uint16_t idx = (key != nullptr) ? internal.FindChildIndex(*key, comparator_)
                                        : static_cast<uint16_t>(0);
        current = internal.ChildAt(idx);
    }
}

StatusOr<BPlusTree::InsertResult>
BPlusTree::InsertRecursive(page_id_t page_id, const Slice& key, const Slice& value) {
    StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, page_id);
    if (!guard_or.ok()) {
        return guard_or.status();
    }
    PageGuard guard = std::move(guard_or.value());

    if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
        BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
        Status s = leaf.Insert(key, value, comparator_);
        if (s.ok()) {
            guard.MarkDirty();
            return InsertResult{};
        }
        if (s.code() != Status::Code::kResourceExhausted) {
            return s;
        }
        return SplitLeafAndInsert(guard, page_id, key, value);
    }

    BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
    uint16_t child_idx = internal.FindChildIndex(key, comparator_);
    page_id_t child_id = internal.ChildAt(child_idx);

    StatusOr<InsertResult> child_result_or = InsertRecursive(child_id, key, value);
    if (!child_result_or.ok()) {
        return child_result_or.status();
    }
    InsertResult child_result = child_result_or.value();
    if (!child_result.split_occurred) {
        return InsertResult{};
    }

    Status s = internal.InsertEntry(
        child_idx, Slice(child_result.split_key), child_result.new_right_child_id);
    if (s.ok()) {
        guard.MarkDirty();
        return InsertResult{};
    }
    if (s.code() != Status::Code::kResourceExhausted) {
        return s;
    }

    return SplitInternalAndInsert(
        guard, page_id, child_idx, Slice(child_result.split_key), child_result.new_right_child_id);
}

StatusOr<BPlusTree::InsertResult> BPlusTree::SplitLeafAndInsert(PageGuard& leaf_guard,
                                                                page_id_t page_id,
                                                                const Slice& key,
                                                                const Slice& value) {
    BPlusTreeLeafPage leaf(leaf_guard.mutable_data(), buffer_pool_manager_->page_size());

    page_id_t old_next = leaf.next_leaf_page_id();
    uint16_t n = leaf.num_keys();
    std::vector<std::pair<std::string, std::string>> all;
    all.reserve(static_cast<size_t>(n) + 1);
    for (uint16_t i = 0; i < n; ++i) {
        all.emplace_back(leaf.KeyAt(i).ToString(), leaf.ValueAt(i).ToString());
    }
    auto insert_pos =
        std::lower_bound(all.begin(),
                         all.end(),
                         key,
                         [this](const std::pair<std::string, std::string>& p, const Slice& k) {
                             return comparator_(Slice(p.first), k) < 0;
                         });
    all.insert(insert_pos, {key.ToString(), value.ToString()});

    uint16_t total = static_cast<uint16_t>(all.size());
    uint16_t mid = total / 2;

    page_id_t new_leaf_id;
    StatusOr<char*> new_data_or = free_page_manager_->AllocateAndPinPage(&new_leaf_id);
    if (!new_data_or.ok()) {
        return new_data_or.status();
    }
    PageGuard new_guard(buffer_pool_manager_, new_leaf_id, new_data_or.value());

    BPlusTreeLeafPage::InitNewPage(
        new_guard.mutable_data(), buffer_pool_manager_->page_size(), new_leaf_id);
    BPlusTreeLeafPage new_leaf(new_guard.mutable_data(), buffer_pool_manager_->page_size());

    // Re-initialize the ORIGINAL page and rebuild it as the LEFT half.
    BPlusTreeLeafPage::InitNewPage(
        leaf_guard.mutable_data(), buffer_pool_manager_->page_size(), page_id);
    BPlusTreeLeafPage left_leaf(leaf_guard.mutable_data(), buffer_pool_manager_->page_size());

    for (uint16_t i = 0; i < mid; ++i) {
        Status s = left_leaf.Insert(Slice(all[i].first), Slice(all[i].second), comparator_);
        if (!s.ok()) {
            return s;
        }
    }
    for (uint16_t i = mid; i < total; ++i) {
        Status s = new_leaf.Insert(Slice(all[i].first), Slice(all[i].second), comparator_);
        if (!s.ok()) {
            return s;
        }
    }

    new_leaf.SetNextLeafPageId(old_next);
    left_leaf.SetNextLeafPageId(new_leaf_id);

    leaf_guard.MarkDirty();
    new_guard.MarkDirty();

    std::string separator = all[mid].first; // first key of the right half
    return InsertResult{true, separator, new_leaf_id};
}

StatusOr<BPlusTree::InsertResult> BPlusTree::SplitInternalAndInsert(PageGuard& internal_guard,
                                                                    page_id_t page_id, 
                                                                    uint16_t insert_idx,
                                                                    const Slice& new_key,
                                                                    page_id_t new_child_id) {
    BPlusTreeInternalPage internal(internal_guard.mutable_data(),
                                   buffer_pool_manager_->page_size());
    uint16_t old_num_keys = internal.num_keys();

    std::vector<page_id_t> children;
    std::vector<std::string> keys;
    children.reserve(static_cast<size_t>(old_num_keys) + 2);
    keys.reserve(static_cast<size_t>(old_num_keys) + 1);

    children.push_back(internal.ChildAt(0));
    for (uint16_t i = 0; i < old_num_keys; ++i) {
        keys.push_back(internal.KeyAt(i).ToString());
        children.push_back(internal.ChildAt(static_cast<uint16_t>(i + 1)));
    }
    keys.insert(keys.begin() + insert_idx, new_key.ToString());
    children.insert(children.begin() + insert_idx + 1, new_child_id);

    uint16_t total_keys = static_cast<uint16_t>(keys.size()); // == old_num_keys + 1
    uint16_t mid = total_keys / 2;
    std::string promoted_key = keys[mid];

    page_id_t new_internal_id;
    StatusOr<char*> new_data_or = free_page_manager_->AllocateAndPinPage(&new_internal_id);
    if (!new_data_or.ok()) {
        return new_data_or.status();
    }
    PageGuard new_guard(buffer_pool_manager_, new_internal_id, new_data_or.value());

    BPlusTreeInternalPage::InitNewPage(new_guard.mutable_data(),
                                       buffer_pool_manager_->page_size(),
                                       new_internal_id,
                                       children[mid + 1]);
    BPlusTreeInternalPage new_internal(new_guard.mutable_data(), buffer_pool_manager_->page_size());
    for (uint16_t i = static_cast<uint16_t>(mid + 1); i < total_keys; ++i) {
        Status s = new_internal.InsertEntry(
            static_cast<uint16_t>(i - (mid + 1)), Slice(keys[i]), children[i + 1]);
        if (!s.ok()) {
            return s;
        }
    }

    // Left half: children[0 .. mid], keys[0 .. mid).
    BPlusTreeInternalPage::InitNewPage(
        internal_guard.mutable_data(), buffer_pool_manager_->page_size(), page_id, children[0]);
    BPlusTreeInternalPage left_internal(internal_guard.mutable_data(),
                                        buffer_pool_manager_->page_size());
    for (uint16_t i = 0; i < mid; ++i) {
        Status s = left_internal.InsertEntry(i, Slice(keys[i]), children[i + 1]);
        if (!s.ok()) {
            return s;
        }
    }

    internal_guard.MarkDirty();
    new_guard.MarkDirty();

    return InsertResult{true, promoted_key, new_internal_id};
}

StatusOr<int> BPlusTree::Height() {
    if (IsEmpty()) {
        return 0;
    }
    int height = 0;
    page_id_t current = disk_manager_->GetRootPageId();
    while (true) {
        StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, current);
        if (!guard_or.ok()) {
            return guard_or.status();
        }
        PageGuard guard = std::move(guard_or.value());
        ++height;
        if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
            return height;
        }
        BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
        current = internal.ChildAt(0); // leftmost path is representative: height is uniform
    }
}

Status BPlusTree::Verify() {
    if (IsEmpty()) {
        return Status::OK();
    }
    int leaf_depth = -1;
    return VerifyRecursive(disk_manager_->GetRootPageId(), nullptr, nullptr, 0, &leaf_depth);
}

Status BPlusTree::VerifyRecursive(
    page_id_t page_id, 
    const Slice* min_key, 
    const Slice* max_key, 
    int depth, 
    int* out_leaf_depth) {
    StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, page_id);
    if (!guard_or.ok()) {
        return guard_or.status();
    }
    PageGuard guard = std::move(guard_or.value());

    if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
        if (*out_leaf_depth == -1) {
            *out_leaf_depth = depth;
        } else if (*out_leaf_depth != depth) {
            return Status::Corruption("BPlusTree::Verify: leaf page " + std::to_string(page_id) +
                                      " at depth " + std::to_string(depth) +
                                      " but other leaves are at depth " +
                                      std::to_string(*out_leaf_depth) + " - tree is unbalanced");
        }
        BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
        uint16_t n = leaf.num_keys();
        if (n == 0) {
            return Status::Corruption(
                "BPlusTree::Verify: leaf " + std::to_string(page_id) +
                " has zero keys - Remove()/rebalancing should never leave an empty "
                "leaf reachable (root-emptying is handled by deallocating the page "
                "entirely, not leaving a degenerate leaf behind)");
        }
        for (uint16_t i = 0; i < n; ++i) {
            Slice k = leaf.KeyAt(i);
            if (i > 0 && comparator_(leaf.KeyAt(static_cast<uint16_t>(i - 1)), k) >= 0) {
                return Status::Corruption("BPlusTree::Verify: leaf " + std::to_string(page_id) +
                                          " keys not strictly sorted at index " +
                                          std::to_string(i));
            }
            if (min_key != nullptr && comparator_(k, *min_key) < 0) {
                return Status::Corruption("BPlusTree::Verify: leaf " + std::to_string(page_id) +
                                          " key " + k.ToString() +
                                          " is below its subtree's lower bound");
            }
            if (max_key != nullptr && comparator_(k, *max_key) >= 0) {
                return Status::Corruption("BPlusTree::Verify: leaf " + std::to_string(page_id) +
                                          " key " + k.ToString() +
                                          " is at/above its subtree's upper bound");
            }
        }
        return Status::OK();
    }

    BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
    uint16_t n = internal.num_keys();
    if (n == 0) {
        return Status::Corruption("BPlusTree::Verify: internal page " + std::to_string(page_id) +
                                  " has zero keys, should be impossible post-split");
    }
    for (uint16_t i = 0; i < n; ++i) {
        Slice k = internal.KeyAt(i);
        if (i > 0 && comparator_(internal.KeyAt(static_cast<uint16_t>(i - 1)), k) >= 0) {
            return Status::Corruption("BPlusTree::Verify: internal " + std::to_string(page_id) +
                                      " keys not strictly sorted at index " + std::to_string(i));
        }
        if (min_key != nullptr && comparator_(k, *min_key) < 0) {
            return Status::Corruption("BPlusTree::Verify: internal " + std::to_string(page_id) +
                                      " key " + k.ToString() +
                                      " is below its subtree's lower bound");
        }
        if (max_key != nullptr && comparator_(k, *max_key) >= 0) {
            return Status::Corruption("BPlusTree::Verify: internal " + std::to_string(page_id) +
                                      " key " + k.ToString() +
                                      " is at/above its subtree's upper bound");
        }
    }

    for (uint16_t i = 0; i <= n; ++i) {
        const Slice* child_min = (i == 0) ? min_key : nullptr;
        const Slice* child_max = (i == n) ? max_key : nullptr;
        Slice lo_storage;
        Slice hi_storage;
        if (i > 0) {
            lo_storage = internal.KeyAt(static_cast<uint16_t>(i - 1));
            child_min = &lo_storage;
        }
        if (i < n) {
            hi_storage = internal.KeyAt(i);
            child_max = &hi_storage;
        }
        Status s =
            VerifyRecursive(internal.ChildAt(i), child_min, child_max, depth + 1, out_leaf_depth);
        if (!s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

StatusOr<std::string> BPlusTree::ToString() {
    if (IsEmpty()) {
        return std::string("(empty tree)\n");
    }
    return ToStringRecursive(disk_manager_->GetRootPageId(), 0);
}

StatusOr<std::string> BPlusTree::ToStringRecursive(page_id_t page_id, int depth) {
    StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, page_id);
    if (!guard_or.ok()) {
        return guard_or.status();
    }
    PageGuard guard = std::move(guard_or.value());
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    std::ostringstream out;

    if (PeekPageType(guard.data()) == PageType::kBTreeLeaf) {
        BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
        out << indent << "Leaf(page=" << page_id << ", keys=[";
        for (uint16_t i = 0; i < leaf.num_keys(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << leaf.KeyAt(i).ToString();
        }
        out << "])\n";
        return out.str();
    }

    BPlusTreeInternalPage internal(guard.mutable_data(), buffer_pool_manager_->page_size());
    out << indent << "Internal(page=" << page_id << ", keys=[";
    for (uint16_t i = 0; i < internal.num_keys(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << internal.KeyAt(i).ToString();
    }
    out << "])\n";
    for (uint16_t i = 0; i <= internal.num_keys(); ++i) {
        StatusOr<std::string> child_str = ToStringRecursive(internal.ChildAt(i), depth + 1);
        if (!child_str.ok()) {
            return child_str.status();
        }
        out << child_str.value();
    }
    return out.str();
}

} // namespace engine
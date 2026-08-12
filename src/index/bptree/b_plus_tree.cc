#include "engine/b_plus_tree.h"
#include "engine/page_guard.h"

namespace engine {

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

    StatusOr<PageGuard> guard_or =
        FetchPageGuarded(buffer_pool_manager_, disk_manager_->GetRootPageId());
    if (!guard_or.ok()) {
        return guard_or.status();
    }
    PageGuard guard = std::move(guard_or.value());

    BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
    BPlusTreeLeafPage::SearchResult result = leaf.FindKey(key, comparator_);
    if (!result.found) {
        return Status::NotFound("BPlusTree::Get: key not found: " + key.ToString());
    }
    return leaf.ValueAt(result.index).ToString(); // copy while guard is alive
}

Status BPlusTree::Insert(const Slice& key, const Slice& value) {
    StatusOr<page_id_t> root_or = GetOrCreateRootLeaf();
    if (!root_or.ok()) {
        return root_or.status();
    }

    StatusOr<PageGuard> guard_or = FetchPageGuarded(buffer_pool_manager_, root_or.value());
    if (!guard_or.ok()) {
        return guard_or.status();
    }
    PageGuard guard = std::move(guard_or.value());

    BPlusTreeLeafPage leaf(guard.mutable_data(), buffer_pool_manager_->page_size());
    Status s = leaf.Insert(key, value, comparator_);
    if (s.ok()) {
        guard.MarkDirty();
    }
    return s;
}

} // namespace engine
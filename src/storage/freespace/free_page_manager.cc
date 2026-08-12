#include <unordered_set>

#include "engine/free_list_node.h"
#include "engine/free_page_manager.h"

namespace engine {

StatusOr<page_id_t> FreePageManager::AllocatePage() {
    page_id_t head = disk_manager_->GetFreeListHead();
    if (head == kInvalidPageId) {
        return disk_manager_->AllocatePage(); // only path that ever grows the file
    }

    StatusOr<char*> data_or = buffer_pool_manager_->FetchPage(head);
    if (!data_or.ok()) {
        return data_or.status();
    }

    StatusOr<FreeListNode> node_or =
        FreeListNode::DeserializeFrom(data_or.value(), buffer_pool_manager_->page_size());
    // Unpin before checking node_or's status. We fetched (pinned) this
    // page regardless of whether it deserializes cleanly.
    Status unpin_s = buffer_pool_manager_->UnpinPage(head, /*is_dirty=*/false);
    if (!node_or.ok()) {
        return node_or.status();
    }
    if (!unpin_s.ok()) {
        return unpin_s;
    }

    Status set_s = disk_manager_->SetFreeListHead(node_or.value().next_free_page_id);
    if (!set_s.ok()) {
        return set_s;
    }

    return head;
}

StatusOr<char*> FreePageManager::AllocateAndPinPage(page_id_t* out_page_id) {
    StatusOr<page_id_t> id_or = AllocatePage();
    if (!id_or.ok()) {
        return id_or.status();
    }
    *out_page_id = id_or.value();
    return buffer_pool_manager_->NewPageWithId(*out_page_id);
}

Status FreePageManager::DeallocatePage(page_id_t page_id) {
    if (buffer_pool_manager_->GetPinCount(page_id) > 0) {
        return Status::InvalidArgument(
            "FreePageManager::DeallocatePage: page " + std::to_string(page_id) +
            " is still pinned; unpin every reference before deallocating");
    }

    page_id_t old_head = disk_manager_->GetFreeListHead();
    if (page_id == old_head) {
        return Status::InvalidArgument("FreePageManager::DeallocatePage: page " +
                                       std::to_string(page_id) +
                                       " is already the free list head; likely double-free");
    }

    StatusOr<char*> data_or = buffer_pool_manager_->FetchPage(page_id);
    if (!data_or.ok()) {
        return data_or.status();
    }

    FreeListNode::SerializeTo(data_or.value(), buffer_pool_manager_->page_size(), old_head);

    Status unpin_s = buffer_pool_manager_->UnpinPage(page_id, /*is_dirty=*/true);
    if (!unpin_s.ok()) {
        return unpin_s;
    }

    return disk_manager_->SetFreeListHead(page_id);
}

StatusOr<std::vector<page_id_t>> FreePageManager::DebugWalkFreeList(size_t max_length) const {
    std::vector<page_id_t> result;
    std::unordered_set<page_id_t> visited;

    page_id_t current = disk_manager_->GetFreeListHead();
    while (current != kInvalidPageId) {
        if (result.size() >= max_length) {
            return Status::Corruption("free list exceeds max_length (" +
                                      std::to_string(max_length) +
                                      "); likely an undetected cycle or runaway chain");
        }
        if (visited.contains(current)) {
            return Status::Corruption("free list contains a cycle at page_id " +
                                      std::to_string(current));
        }
        visited.insert(current);

        StatusOr<char*> data_or = buffer_pool_manager_->FetchPage(current);
        if (!data_or.ok()) {
            return data_or.status();
        }

        StatusOr<FreeListNode> node_or =
            FreeListNode::DeserializeFrom(data_or.value(), buffer_pool_manager_->page_size());
        Status unpin_s = buffer_pool_manager_->UnpinPage(current, /*is_dirty=*/false);
        if (!node_or.ok()) {
            return node_or.status();
        }
        if (!unpin_s.ok()) {
            return unpin_s;
        }

        result.push_back(current);
        current = node_or.value().next_free_page_id;
    }
    return result;
}

} // namespace engine
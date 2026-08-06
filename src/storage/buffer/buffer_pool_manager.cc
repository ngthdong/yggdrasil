#include "engine/buffer_pool_manager.h"

#include <cassert>
#include <cstring>
#include <sstream>

namespace engine {

BufferPoolManager::BufferPoolManager(DiskManager* disk_manager, size_t num_frames)
    : disk_manager_(disk_manager), page_size_(disk_manager->page_size()) {
    assert(num_frames > 0);
    pool_memory_.assign(static_cast<size_t>(page_size_) * num_frames, 0);
    frames_.resize(num_frames);
    free_list_.reserve(num_frames);
    for (size_t i = 0; i < num_frames; ++i) {
        frames_[i].data = pool_memory_.data() + i * page_size_;
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

StatusOr<char*> BufferPoolManager::FetchPage(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Frame& frame = frames_[static_cast<size_t>(it->second)];
        frame.pin_count += 1;
        return frame.data;
    }

    if (free_list_.empty()) {
        return Status::ResourceExhausted(
            "BufferPoolManager::FetchPage: pool full and no eviction policy yet ");
    }

    frame_id_t frame_id = free_list_.back();
    Frame& frame = frames_[static_cast<size_t>(frame_id)];

    Status s = disk_manager_->ReadPage(page_id, frame.data);
    if (!s.ok()) {
        return s;
    }

    free_list_.pop_back();
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    page_table_[page_id] = frame_id;
    return frame.data;
}

StatusOr<char*> BufferPoolManager::NewPage(page_id_t* out_page_id) {
    if (free_list_.empty()) {
        return Status::ResourceExhausted(
            "BufferPoolManager::NewPage: pool full and no eviction policy yet");
    }

    StatusOr<page_id_t> id_or = disk_manager_->AllocatePage();
    if (!id_or.ok()) {
        return id_or.status();
    }
    page_id_t page_id = id_or.value();

    frame_id_t frame_id = free_list_.back();
    free_list_.pop_back();
    Frame& frame = frames_[static_cast<size_t>(frame_id)];

    std::memset(frame.data, 0, page_size_);
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;

    page_table_[page_id] = frame_id;
    *out_page_id = page_id;
    return frame.data;
}

Status BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::InvalidArgument("BufferPoolManager::UnpinPage: page_id " +
                                       std::to_string(page_id) + " is not currently resident");
    }
    Frame& frame = frames_[static_cast<size_t>(it->second)];
    if (frame.pin_count == 0) {
        return Status::InvalidArgument("BufferPoolManager::UnpinPage: page_id " +
                                       std::to_string(page_id) +
                                       " has pin_count already 0 -- double-unpin bug in caller");
    }
    frame.pin_count -= 1;
    if (is_dirty) {
        frame.is_dirty = true; // sticky: never cleared here
    }
    return Status::OK();
}

Status BufferPoolManager::FlushPage(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::InvalidArgument("BufferPoolManager::FlushPage: page_id " +
                                       std::to_string(page_id) + " is not resident");
    }
    Frame& frame = frames_[static_cast<size_t>(it->second)];
    if (!frame.is_dirty) {
        return Status::OK();
    }

    Status s = disk_manager_->WritePage(page_id, frame.data);
    if (!s.ok()) {
        return s;
    }
    frame.is_dirty = false;
    return Status::OK();
}

Status BufferPoolManager::FlushAllPages() {
    Status first_error = Status::OK();
    for (const auto& [page_id, frame_id] : page_table_) {
        Status s = FlushPage(page_id);
        if (!s.ok() && first_error.ok()) {
            first_error = s;
        }
    }
    return first_error;
}

size_t BufferPoolManager::GetPinCount(page_id_t page_id) const {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return 0;
    }
    return frames_[static_cast<size_t>(it->second)].pin_count;
}

bool BufferPoolManager::IsResident(page_id_t page_id) const {
    return page_table_.find(page_id) != page_table_.end();
}

std::string BufferPoolManager::DebugString() const {
    std::ostringstream out;
    out << "BufferPoolManager: " << frames_.size() << " frames, " << free_list_.size() << " free, "
        << page_table_.size() << " resident\n";
    for (const auto& [page_id, frame_id] : page_table_) {
        const Frame& frame = frames_[static_cast<size_t>(frame_id)];
        out << "  page " << page_id << " -> frame " << frame_id << " pin_count=" << frame.pin_count
            << " dirty=" << (frame.is_dirty ? "yes" : "no") << "\n";
    }
    return out.str();
}

} // namespace engine
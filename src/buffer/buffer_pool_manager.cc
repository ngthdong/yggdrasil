#include "engine/buffer_pool_manager.h"

#include <cassert>
#include <cstring>
#include <sstream>

#include "engine/clock_replacer.h"
#include "engine/lru_replacer.h"

namespace engine {

BufferPoolManager::BufferPoolManager(DiskManager* disk_manager,
                                     size_t num_frames,
                                     ReplacerPolicy policy)
    : disk_manager_(disk_manager), page_size_(disk_manager->page_size()) {
    assert(num_frames > 0);
    pool_memory_.assign(static_cast<size_t>(page_size_) * num_frames, 0);
    frames_.resize(num_frames);
    free_list_.reserve(num_frames);
    for (size_t i = 0; i < num_frames; ++i) {
        frames_[i].data = pool_memory_.data() + i * page_size_;
        free_list_.push_back(static_cast<frame_id_t>(i));
    }

    if (policy == ReplacerPolicy::kClock) {
        replacer_ = std::make_unique<ClockReplacer>(num_frames);
    } else {
        replacer_ = std::make_unique<LRUReplacer>(num_frames);
    }
}

StatusOr<frame_id_t> BufferPoolManager::GetFreeFrame() {
    if (!free_list_.empty()) {
        frame_id_t frame_id = free_list_.back();
        free_list_.pop_back();
        return frame_id;
    }

    frame_id_t victim = kInvalidFrameId;
    if (!replacer_->Victim(&victim)) {
        return Status::ResourceExhausted(
            "BufferPoolManager: no free frame and nothing evictable -- every "
            "resident page is currently pinned");
    }

    Frame& frame = frames_[static_cast<size_t>(victim)];
    if (frame.is_dirty) {
        Status s = disk_manager_->WritePage(frame.page_id, frame.data);
        if (!s.ok()) {
            replacer_->Unpin(victim);
            return s;
        }
        frame.is_dirty = false;
    }
    page_table_.erase(frame.page_id);
    return victim;
}

StatusOr<char*> BufferPoolManager::FetchPage(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Frame& frame = frames_[static_cast<size_t>(frame_id)];
        if (frame.pin_count == 0) {
            replacer_->Pin(frame_id);
        }
        frame.pin_count += 1;
        hit_count_ += 1;
        return frame.data;
    }

    miss_count_ += 1;
    StatusOr<frame_id_t> frame_id_or = GetFreeFrame();
    if (!frame_id_or.ok()) {
        return frame_id_or.status();
    }
    frame_id_t frame_id = frame_id_or.value();
    Frame& frame = frames_[static_cast<size_t>(frame_id)];

    Status s = disk_manager_->ReadPage(page_id, frame.data);
    if (!s.ok()) {
        free_list_.push_back(frame_id);
        return s;
    }

    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id); // newly loaded and pinned -> not evictable
    return frame.data;
}

StatusOr<char*> BufferPoolManager::ClaimFrameForNewPage(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Frame& frame = frames_[static_cast<size_t>(frame_id)];
        if (frame.pin_count == 0) {
            replacer_->Pin(frame_id); // transitioning back to pinned
        }
        std::memset(frame.data, 0, page_size_);
        frame.pin_count += 1;
        frame.is_dirty = false; // fresh "new page" content; nothing to flush yet
        return frame.data;
    }

    StatusOr<frame_id_t> frame_id_or = GetFreeFrame();
    if (!frame_id_or.ok()) {
        return frame_id_or.status();
    }
    frame_id_t frame_id = frame_id_or.value();

    Frame& frame = frames_[static_cast<size_t>(frame_id)];
    std::memset(frame.data, 0, page_size_);
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);
    return frame.data;
}

StatusOr<char*> BufferPoolManager::NewPage(page_id_t* out_page_id) {
    StatusOr<page_id_t> id_or = disk_manager_->AllocatePage();
    if (!id_or.ok()) {
        return id_or.status();
    }
    page_id_t page_id = id_or.value();

    StatusOr<char*> data_or = ClaimFrameForNewPage(page_id);
    if (!data_or.ok()) {
        return data_or.status();
    }
    *out_page_id = page_id;
    return data_or.value();
}

StatusOr<char*> BufferPoolManager::NewPageWithId(page_id_t page_id) {
    return ClaimFrameForNewPage(page_id);
}

Status BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return Status::InvalidArgument("BufferPoolManager::UnpinPage: page_id " +
                                       std::to_string(page_id) + " is not currently resident");
    }
    frame_id_t frame_id = it->second;
    Frame& frame = frames_[static_cast<size_t>(frame_id)];
    if (frame.pin_count == 0) {
        return Status::InvalidArgument("BufferPoolManager::UnpinPage: page_id " +
                                       std::to_string(page_id) +
                                       " has pin_count already 0, double-unpin bug in caller");
    }
    frame.pin_count -= 1;
    if (is_dirty) {
        frame.is_dirty = true;
    }
    if (frame.pin_count == 0) {
        replacer_->Unpin(frame_id); // now, and only now, eligible for eviction
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
        << replacer_->Size() << " evictable, " << page_table_.size() << " resident\n";
    for (const auto& [page_id, frame_id] : page_table_) {
        const Frame& frame = frames_[static_cast<size_t>(frame_id)];
        out << "  page " << page_id << " -> frame " << frame_id << " pin_count=" << frame.pin_count
            << " dirty=" << (frame.is_dirty ? "yes" : "no") << "\n";
    }
    return out.str();
}

} // namespace engine
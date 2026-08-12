#include "engine/lru_replacer.h"

namespace engine {

LRUReplacer::LRUReplacer(size_t /*num_frames*/){};

bool LRUReplacer::Victim(frame_id_t* frame_id) {
    if (lru_list_.empty()) {
        return false;
    }
    *frame_id = lru_list_.front();
    position_.erase(lru_list_.front());
    lru_list_.pop_front();
    return true;
}

void LRUReplacer::Pin(frame_id_t frame_id) {
    auto it = position_.find(frame_id);
    if (it != position_.end()) {
        lru_list_.erase(it->second);
        position_.erase(it);
    }
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
    if (position_.find(frame_id) != position_.end()) {
        return;
    }
    lru_list_.push_back(frame_id);
    position_[frame_id] = std::prev(lru_list_.end());
}

} // namespace engine
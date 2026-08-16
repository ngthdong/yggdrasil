#pragma once

#include <list>
#include <unordered_map>

#include "engine/replacer.h"

namespace engine {

// LRU replacement policy.
//
// Keeps evictable frames ordered by recency:
//   - Front  -> least recently used (next victim).
//   - Back   -> most recently used.
//
// position_ stores each frame's position in lru_list_, allowing O(1)
// removal and update.
//
// Example:
//   LRU list:       A  B  C  D
//                   ^        ^
//                 victim    newest
//
//   If C is accessed:
//   LRU list:       A  B  D  C
//
//   Victim() evicts A, the least recently used frame.
//
// Pin() removes a frame from the LRU list because it cannot be evicted.
// Unpin() adds the frame back as the most recently used frame.
class LRUReplacer : public Replacer {
  public:
    explicit LRUReplacer(size_t num_frames);

    bool Victim(frame_id_t* frame_id) override;

    void Pin(frame_id_t frame_id) override;

    void Unpin(frame_id_t frame_id) override;

    size_t Size() const override {
        return lru_list_.size();
    }

  private:
    std::list<frame_id_t> lru_list_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> position_;
};

} // namespace engine
#pragma once

#include <list>
#include <unordered_map>

#include "engine/replacer.h"

namespace engine {

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
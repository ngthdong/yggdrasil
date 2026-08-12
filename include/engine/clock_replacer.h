#pragma once

#include <vector>

#include "engine/replacer.h"

namespace engine {

class ClockReplacer : public Replacer {
  public:
    explicit ClockReplacer(size_t num_frames);

    bool Victim(frame_id_t* frame_id) override;

    void Pin(frame_id_t frame_id) override;

    void Unpin(frame_id_t frame_id) override;

    size_t Size() const override {
        return evictable_count_;
    }

  private:
    std::vector<bool> reference_bit_;
    std::vector<bool> evictable_;
    size_t clock_hand_ = 0;
    size_t evictable_count_ = 0;
    size_t num_frames_;
};
} // namespace engine
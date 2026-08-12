#include "engine/clock_replacer.h"

namespace engine {

ClockReplacer::ClockReplacer(size_t num_frames)
    : reference_bit_(num_frames, false), evictable_(num_frames, false), num_frames_(num_frames) {}

bool ClockReplacer::Victim(frame_id_t* frame_id) {
    if (evictable_count_ == 0) {
        return false;
    }
    size_t steps = 0;
    size_t max_steps = num_frames_ * 2 + 1;
    while (steps < max_steps) {
        if (evictable_[clock_hand_]) {
            if (reference_bit_[clock_hand_]) {
                reference_bit_[clock_hand_] = false;
            } else {
                *frame_id = static_cast<frame_id_t>(clock_hand_);
                evictable_[clock_hand_] = false;
                evictable_count_ -= 1;
                clock_hand_ = (clock_hand_ + 1) % num_frames_;
                return true;
            }
        }
        clock_hand_ = (clock_hand_ + 1) % num_frames_;
        steps += 1;
    }
    return false;
}

void ClockReplacer::Pin(frame_id_t frame_id) {
    auto idx = static_cast<size_t>(frame_id);
    if (evictable_[idx]) {
        evictable_[idx] = false;
        evictable_count_ -= 1;
    }
}

void ClockReplacer::Unpin(frame_id_t frame_id) {
    auto idx = static_cast<size_t>(frame_id);
    if (!evictable_[idx]) {
        evictable_[idx] = true;
        evictable_count_ += 1;
    }
    reference_bit_[idx] = true;
}

} // namespace engine
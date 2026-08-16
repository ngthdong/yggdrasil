#pragma once

#include <vector>

#include "engine/replacer.h"

namespace engine {

// Clock replacement policy.
//
// Frames are treated as a circular list. Victim() scans from clock_hand_:
//   - Skip non-evictable frames.
//   - If reference bit is set, clear it and give the frame a second chance.
//   - If reference bit is clear, evict the frame.
//
// Example:
//   Frames:        A  B  C  D  E
//   Evictable:     Y  Y  Y  N  Y
//   Reference:     1  1  0  1  1
//   clock_hand_:   ^
//
//   1. A has reference=1 -> clear it and give A a second chance.
//   2. B has reference=1 -> clear it and give B a second chance.
//   3. C has reference=0 -> evict C.
//
//   After eviction:
//   Frames:        A  B  _  D  E
//   clock_hand_ points to D, the frame after the victim.
//
// If the scan reaches the end, it wraps around to the beginning,
// forming a circular scan.
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
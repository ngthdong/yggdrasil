#pragma once

#include "engine/config.h"
#include <cstddef>

namespace engine {

// Replacer is an interface for page replacement policies.
// It is used by the BufferPoolManager to determine which page to evict when the buffer pool is
// full.
class Replacer {
  public:
    virtual ~Replacer() = default;

    // Selects a victim page to evict and returns true if successful.
    virtual bool Victim(frame_id_t* frame_id) = 0;

    // marks frame_id as pinned, meaning it cannot be evicted.
    virtual void Pin(frame_id_t frame_id) = 0;

    // marks frame_id as unpinned, meaning it can be evicted.
    virtual void Unpin(frame_id_t frame_id) = 0;

    // Returns the number of unpinned pages in the replacer.
    virtual size_t Size() const = 0;
};

} // namespace engine
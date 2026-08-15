#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

struct DBStats {
    uint32_t page_count = 0;
    size_t buffer_pool_capacity_frames = 0;
    size_t buffer_pool_resident_frames = 0;
    uint64_t buffer_pool_hits = 0;
    uint64_t buffer_pool_misses = 0;
    int tree_height = 0;

    double BufferPoolHitRate() const {
        uint64_t total = buffer_pool_hits + buffer_pool_misses;
        return total == 0 ? 0.0
                          : static_cast<double>(buffer_pool_hits) / static_cast<double>(total);
    }
};

} // namespace engine
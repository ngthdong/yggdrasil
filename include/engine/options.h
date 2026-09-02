#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "engine/lock_manager.h"
#include "engine/status.h"

namespace engine {

struct Options {
    std::string path;

    uint32_t page_size = 4096;

    size_t buffer_pool_frames = 1024;

    bool create_if_missing = true;

    bool sync_on_commit = true;

    DeadlockPolicy deadlock_policy = DeadlockPolicy::kWoundWait;

    // Only used when deadlock_policy == DeadlockPolicy::kDetection.
    std::chrono::milliseconds deadlock_detection_interval = std::chrono::milliseconds(50);

    Status Validate() const {
        if (path.empty()) {
            return Status::InvalidArgument("Options.path must not be empty");
        }
        if (page_size < 512 || (page_size & (page_size - 1)) != 0) {
            return Status::InvalidArgument("Options.page_size must be a power of two >= 512, got " +
                                           std::to_string(page_size));
        }
        if (buffer_pool_frames == 0) {
            return Status::InvalidArgument("Options.buffer_pool_frames must be > 0");
        }
        return Status::OK();
    }
};

} // namespace engine

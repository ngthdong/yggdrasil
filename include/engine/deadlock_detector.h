#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "engine/lock_manager.h"

namespace engine {

// Runs deadlock detection periodically on a dedicated background thread.
//
// The detector is effective only when the LockManager is configured with
// DeadlockPolicy::kDetection. For kWoundWait, RunDetectionCycle() is a
// no-op; constructing a detector in that configuration is therefore
// redundant but remains safe.
class DeadlockDetector {
  public:
    explicit DeadlockDetector(LockManager* lock_manager, std::chrono::milliseconds scan_interval)
        : lock_manager_(lock_manager), scan_interval_(scan_interval) {}

    ~DeadlockDetector() {
        Stop();
    }
    DeadlockDetector(const DeadlockDetector&) = delete;
    DeadlockDetector& operator=(const DeadlockDetector&) = delete;

    void Start();
    void Stop();

    bool is_running() const;

  private:
    void RunLoop();

    LockManager* lock_manager_;
    std::chrono::milliseconds scan_interval_;
    std::thread thread_;

    mutable std::mutex control_mutex_;
    std::condition_variable control_cv_;
    bool running_ = false;
};

} // namespace engine
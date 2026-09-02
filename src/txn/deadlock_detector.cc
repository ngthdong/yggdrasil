#include "engine/deadlock_detector.h"

namespace engine {

void DeadlockDetector::Start() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    thread_ = std::thread(&DeadlockDetector::RunLoop, this);
}

void DeadlockDetector::Stop() {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    control_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool DeadlockDetector::is_running() const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return running_;
}

void DeadlockDetector::RunLoop() {
    std::unique_lock<std::mutex> lock(control_mutex_);
    while (running_) {
        // Use wait_for() with a predicate instead of sleep_for() so that Stop()
        // can wake the detector immediately via notify_all(). This avoids
        // delaying shutdown until the current scan interval expires.
        control_cv_.wait_for(lock, scan_interval_, [this] { return !running_; });
        if (!running_) {
            break;
        }
        lock.unlock();
        lock_manager_->RunDetectionCycle();
        lock.lock();
    }
}

} // namespace engine
#include "engine/lock_manager.h"

#include <algorithm>

namespace engine {

bool LockManager::HasConflict(const std::list<LockRequest>& granted,
                              txn_id_t requester,
                              LockMode mode) {
    return std::ranges::any_of(granted, [&](const LockRequest& req) {
        if (req.txn_id == requester) {
            return false;
        }
        if (mode == LockMode::kShared && req.mode == LockMode::kShared) {
            return false;
        }
        return true;
    });
}

Status LockManager::AcquireLock(txn_id_t txn_id, const std::string& resource, LockMode mode) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (aborted_.contains(txn_id)) {
        return Status::Aborted("LockManager: transaction " + std::to_string(txn_id) +
                               " was wounded");
    }

    std::list<LockRequest>& granted = lock_table_[resource];

    for (const auto& req : granted) {
        if (req.txn_id == txn_id) {
            if (req.mode == LockMode::kExclusive || req.mode == mode) {
                return Status::OK(); // already holds this mode or stronger
            }
            break;
        }
    }

    while (HasConflict(granted, txn_id, mode)) {
        for (const auto& req : granted) {
            if (req.txn_id == txn_id) {
                continue;
            }
            if (mode == LockMode::kShared && req.mode == LockMode::kShared) {
                continue;
            }
            if (txn_id < req.txn_id) {
                aborted_.insert(req.txn_id);
            }
        }
        cv_.notify_all();
        cv_.wait(lock);
        if (aborted_.contains(txn_id)) {
            return Status::Aborted("LockManager: transaction " + std::to_string(txn_id) +
                                   " was wounded while waiting");
        }
    }

    granted.remove_if([txn_id](const LockRequest& req) { return req.txn_id == txn_id; });
    granted.push_back(LockRequest{txn_id, mode});
    return Status::OK();
}

void LockManager::ReleaseAllLocks(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [resourse, granted] : lock_table_) {
        granted.remove_if([txn_id](const LockRequest& req) { return req.txn_id == txn_id; });
    }
    aborted_.erase(txn_id);
    cv_.notify_all();
}

bool LockManager::IsAborted(txn_id_t txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_.contains(txn_id);
}

} // namespace engine
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
        if (policy_ == DeadlockPolicy::kWoundWait) {
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
        }
        waiting_[txn_id] = WaitState{resource, mode};
        cv_.notify_all();
        cv_.wait(lock);
        waiting_.erase(txn_id);
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

bool LockManager::FindCycle(txn_id_t start,
                            const std::unordered_map<txn_id_t, std::vector<txn_id_t>>& graph,
                            std::unordered_set<txn_id_t>* visited,
                            std::vector<txn_id_t>* path,
                            std::unordered_set<txn_id_t>* in_path,
                            std::vector<txn_id_t>* cycle_out) const {
    visited->insert(start);
    in_path->insert(start);
    path->push_back(start);
    auto it = graph.find(start);
    if (it != graph.end()) {
        for (txn_id_t next : it->second) {
            if (in_path->count(next) > 0) {
                auto cs = std::find(path->begin(), path->end(), next);
                cycle_out->assign(cs, path->end());
                return true;
            }
            if (visited->count(next) == 0 &&
                FindCycle(next, graph, visited, path, in_path, cycle_out)) {
                return true;
            }
        }
    }
    path->pop_back();
    in_path->erase(start);
    return false;
}

void LockManager::RunDetectionCycle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (policy_ != DeadlockPolicy::kDetection) {
        return;
    }
    std::unordered_map<txn_id_t, std::vector<txn_id_t>> graph;
    for (const auto& [waiter_id, wait] : waiting_) {
        auto lt_it = lock_table_.find(wait.resourse);
        if (lt_it == lock_table_.end()) {
            continue;
        }
        for (const auto& req : lt_it->second) {
            if (req.txn_id == waiter_id) {
                continue;
            }
            if (wait.mode == LockMode::kShared && req.mode == LockMode::kShared) {
                continue;
            }
            graph[waiter_id].push_back(req.txn_id);
        }
    }
    std::unordered_set<txn_id_t> visited;
    for (const auto& [node, edges] : graph) {
        if (visited.contains(node)) {
            continue;
        }
        std::vector<txn_id_t> path;
        std::vector<txn_id_t> cycle;
        std::unordered_set<txn_id_t> in_path;
        if (FindCycle(node, graph, &visited, &path, &in_path, &cycle)) {
            txn_id_t victim = *std::max_element(cycle.begin(), cycle.end());
            aborted_.insert(victim);
            cv_.notify_all();
            return;
        }
    }
}

} // namespace engine
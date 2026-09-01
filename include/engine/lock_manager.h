#pragma once

#include <condition_variable>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "engine/config.h"
#include "engine/status.h"

namespace engine {

enum class LockMode { kShared, kExclusive };

// Coordinates transaction-level locks for concurrent access to shared keys.
//
// Locks are acquired at key granularity and are held for the lifetime of the
// transaction. The manager enforces Strict Two-Phase Locking (Strict 2PL):
// transactions may acquire locks while active, but release them only when
// they commit or abort.
//
// Deadlocks are prevented using the Wound-Wait protocol. Transaction age is
// determined by txn_id; smaller txn_id values represent older transactions.
// When a lock conflict occurs, an older requester aborts the younger holder,
// while a younger requester waits for an older holder.
//
// This age ordering prevents cyclic wait dependencies, so explicit
// deadlock detection is not required.
class LockManager {
  public:
    Status AcquireLock(txn_id_t txn_id, const std::string& resource, LockMode mode);
    void ReleaseAllLocks(txn_id_t txn_id);
    bool IsAborted(txn_id_t txn_id) const;

  private:
    struct LockRequest {
        txn_id_t txn_id;
        LockMode mode;
    };

    static bool
    HasConflict(const std::list<LockRequest>& granted, txn_id_t requester, LockMode mode);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::list<LockRequest>> lock_table_;
    std::unordered_set<txn_id_t> aborted_;
};

} // namespace engine
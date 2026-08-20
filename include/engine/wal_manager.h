#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/config.h"
#include "engine/log_record.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

// Manages the write-ahead log using an in-memory buffer and a separate WAL
// file. Appending a record assigns an LSN and buffers the record; durability
// is guaranteed only after Flush() completes.
//
// Thread-safe and independent of other engine components.
class WalManager {
  public:
    enum class SyncPolicy {
        kEveryFlush,
        kNever,
    };

    // Opens a new WAL session and truncates any existing file at `path`.
    // Existing WAL contents are discarded; recovery is handled separately.
    static StatusOr<std::unique_ptr<WalManager>>
    Open(const std::string& path, SyncPolicy sync_policy = SyncPolicy::kEveryFlush);

    ~WalManager();
    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    // Assigns the next LSN, serializes the record, and appends it to the
    // in-memory pending buffer.
    StatusOr<lsn_t>
    AppendLogRecord(LogRecordType type, page_id_t page_id, const Slice& key, const Slice& value);

    // Blocks until every record up to and including `lsn` is durable
    Status Flush(lsn_t lsn);

    // Thread-safe snapshot of the current durable LSN
    lsn_t durable_lsn() const;

    // Truncates durable WAL data and resets the in-memory buffer.
    // Requires all pending writes to be flushed. LSNs are preserved;
    // only on-disk offsets are reset. Assumes single-writer checkpoints.
    Status RecycleAll();

    Status Shutdown();

  private:
    WalManager(int fd, SyncPolicy sync_policy);

    Status PwriteAll(const char* data, size_t len, off_t offset) const;

    int fd_;
    SyncPolicy sync_policy_;
    bool shutdown_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string buffer_;
    lsn_t next_lsn_ = 1;
    lsn_t highest_appended_lsn_ = 0;
    size_t highest_appended_offset_ = 0;
    lsn_t durable_lsn_ = 0;
    size_t durable_offset_ = 0;
    bool flush_in_progress_ = false;
};

} // namespace engine
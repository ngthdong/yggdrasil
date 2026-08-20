#include "engine/checkpoint_manager.h"
#include "engine/log_record.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {

Status CheckpointManager::TakeCheckpoint() {
    StatusOr<lsn_t> begin_or = wal_manager_->AppendLogRecord(
        LogRecordType::kCheckpointBegin, kInvalidPageId, Slice(""), Slice(""));
    if (!begin_or.ok()) {
        return begin_or.status();
    }
    lsn_t begin_lsn = begin_or.value();

    Status flush_s = buffer_pool_manager_->FlushAllPages();
    if (!flush_s.ok()) {
        return flush_s;
    }

    StatusOr<lsn_t> end_or = wal_manager_->AppendLogRecord(
        LogRecordType::kCheckpointEnd, kInvalidPageId, Slice(std::to_string(begin_lsn)), Slice(""));
    if (!end_or.ok()) {
        return end_or.status();
    }
    lsn_t end_lsn = end_or.value();

    Status wal_flush_s = wal_manager_->Flush(end_lsn);
    if (!wal_flush_s.ok()) {
        return wal_flush_s;
    }

    Status sb_s = disk_manager_->SetLastCheckpointLsn(begin_lsn);
    if (!sb_s.ok()) {
        return sb_s;
    }

    return wal_manager_->RecycleAll();
}

} // namespace engine
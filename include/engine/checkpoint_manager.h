#pragma once

#include "engine/buffer_pool_manager.h"
#include "engine/disk_manager.h"
#include "engine/status.h"
#include "engine/wal_manager.h"

namespace engine {

// Manages database checkpoints by flushing dirty pages, persisting the
// checkpoint LSN, and recycling WAL data no longer needed for recovery.
class CheckpointManager {
  public:
    CheckpointManager(DiskManager* disk_manager,
                      BufferPoolManager* buffer_pool_manager,
                      WalManager* wal_manager)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager),
          wal_manager_(wal_manager) {}

    Status TakeCheckpoint();

  private:
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    WalManager* wal_manager_;
};

} // namespace engine
#pragma once
#include "engine/b_plus_tree.h"
#include "engine/log_record.h"
#include "engine/status.h"
#include <vector>

namespace engine {

// Applies the engine's logical undo rules to a sequence of log records.
//
// This function is shared by crash recovery and live transaction rollback
// to ensure both paths produce identical logical state.
Status ApplyLogicalUndo(BPlusTree* tree, const std::vector<LogRecord>& records);

} // namespace engine
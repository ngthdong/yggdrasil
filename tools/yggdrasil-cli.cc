#include "engine/database.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace {

// State shared across CLI commands for the lifetime of the session.
//
// Owns the database, the active transaction, the pending write batch, and
// open snapshots. Snapshot slots are retained after close so that assigned
// snapshot IDs remain stable throughout the session.
struct CliState {
    explicit CliState(Database& database) : db(database) {}

    Database& db;
    std::optional<Transaction> txn;
    std::optional<WriteBatch> batch;
    std::vector<std::optional<Snapshot>> snapshots;
};

void PrintHelp() {
    std::cout
        << "Key-value commands:\n"
        << "  put <key> <value>          Insert or update a key\n"
        << "  get <key>                  Get a value\n"
        << "  delete <key>               Delete a key (alias: remove)\n"
        << "  scan [--start K] [--end K] Scan keys in sorted order, optionally bounded\n"
        << "\n"
        << "Transactions (put/get/delete route through the active one, if any):\n"
        << "  begin                      Start a transaction\n"
        << "  commit                     Commit the active transaction\n"
        << "  rollback                   Roll back the active transaction\n"
        << "\n"
        << "Atomic write batches:\n"
        << "  batch begin                Start accumulating a batch\n"
        << "  batch put <key> <value>    Add a Put to the pending batch\n"
        << "  batch delete <key>         Add a Delete to the pending batch\n"
        << "  batch commit               Apply the pending batch atomically\n"
        << "  batch cancel               Discard the pending batch\n"
        << "  batch status               Show pending batch size\n"
        << "\n"
        << "Snapshots (point-in-time, read-only):\n"
        << "  snapshot create            Create a snapshot, printing its id\n"
        << "  snapshot get <id> <key>    Read a key as of that snapshot\n"
        << "  snapshot list              List open snapshot ids\n"
        << "  snapshot close <id>        Close a snapshot and free its backing file\n"
        << "\n"
        << "Maintenance and inspection:\n"
        << "  stats                      Show database statistics\n"
        << "  verify [--deep]            Verify database invariants\n"
        << "  checkpoint                 Flush and record a checkpoint\n"
        << "  info                       Show the options this database was opened with\n"
        << "  status                     Show CLI session state (txn/batch/snapshots)\n"
        << "\n"
        << "  help                       Show this help\n"
        << "  exit                       Exit the CLI\n";
}

void PrintUsage() {
    std::cerr
        << "Usage: yggdrasil-cli [flags] <database>\n"
        << "Flags:\n"
        << "  --page-size N              Page size in bytes, power of two >= 512 (default 4096)\n"
        << "  --buffer-pool-frames N     Buffer pool capacity in frames (default 1024)\n"
        << "  --no-create                Fail instead of creating a missing database file\n"
        << "  --no-sync                  Do not fsync the WAL after every commit\n"
        << "  --deadlock-detection       Use background deadlock detection instead of wound-wait\n"
        << "  --detection-interval-ms N  Detection scan interval in ms (default 50)\n";
}

std::vector<std::string> Tokenize(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> tokens;

    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }

    return tokens;
}

void PrintStatus(const Status& status) {
    if (!status.ok()) {
        std::cout << status.ToString() << '\n';
    }
}

// Returns true and fills *out on success. Prints a usage/error message and
// returns false otherwise. text must be a positive, 1-based snapshot id
// naming an open slot.
bool ParseOpenSnapshotId(CliState& state, const std::string& text, size_t* out) {
    unsigned long id = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc() || ptr != text.data() + text.size() || id == 0) {
        std::cout << "Invalid snapshot id: " << text << '\n';
        return false;
    }
    size_t idx = static_cast<size_t>(id) - 1;
    if (idx >= state.snapshots.size() || !state.snapshots[idx]) {
        std::cout << "No open snapshot with id " << text << '\n';
        return false;
    }
    *out = idx;
    return true;
}

bool TransactionIsActive(const CliState& state) {
    return state.txn.has_value() && state.txn->is_active();
}

void ExecutePut(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 3) {
        std::cout << "Usage: put <key> <value>\n";
        return;
    }

    Status status = TransactionIsActive(state) ? state.txn->Put(Slice(args[1]), Slice(args[2]))
                                                : state.db.Put(Slice(args[1]), Slice(args[2]));

    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

void ExecuteGet(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "Usage: get <key>\n";
        return;
    }

    StatusOr<std::string> result =
        TransactionIsActive(state) ? state.txn->Get(Slice(args[1])) : state.db.Get(Slice(args[1]));

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    std::cout << result.value() << '\n';
}

void ExecuteDelete(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "Usage: delete <key>\n";
        return;
    }

    Status status =
        TransactionIsActive(state) ? state.txn->Remove(Slice(args[1])) : state.db.Remove(Slice(args[1]));

    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

void ExecuteScan(CliState& state, const std::vector<std::string>& args) {
    std::optional<std::string> start_key;
    std::optional<std::string> end_key;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--start" && i + 1 < args.size()) {
            start_key = args[++i];
        } else if (args[i] == "--end" && i + 1 < args.size()) {
            end_key = args[++i];
        } else {
            std::cout << "Usage: scan [--start K] [--end K]\n";
            return;
        }
    }

    StatusOr<Database::Iterator> result =
        start_key ? state.db.NewIterator(Slice(*start_key)) : state.db.NewIterator();

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    Database::Iterator iterator = std::move(result.value());
    size_t count = 0;

    while (iterator.Valid()) {
        std::string key = iterator.Key().ToString();
        if (end_key && key > *end_key) {
            break;
        }

        std::cout << key << " = " << iterator.Value().ToString() << '\n';
        ++count;

        Status status = iterator.Next();
        if (!status.ok()) {
            PrintStatus(status);
            return;
        }
    }

    std::cout << count << " key(s)\n";
}

void ExecuteStats(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: stats\n";
        return;
    }

    StatusOr<DBStats> result = state.db.GetStats();

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    const DBStats& stats = result.value();

    std::cout << "Pages:              " << stats.page_count << '\n'
              << "Buffer pool:        " << stats.buffer_pool_capacity_frames << '\n'
              << "Resident frames:    " << stats.buffer_pool_resident_frames << '\n'
              << "Buffer pool hit:    " << stats.BufferPoolHitRate() * 100.0 << "%\n"
              << "Tree height:        " << stats.tree_height << '\n'
              << "Durable LSN:        " << stats.durable_lsn << '\n'
              << "Last checkpoint:    " << stats.last_checkpoint_lsn << '\n';
}

void ExecuteVerify(CliState& state, const std::vector<std::string>& args) {
    bool deep = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--deep") {
            deep = true;
        } else {
            std::cout << "Usage: verify [--deep]\n";
            return;
        }
    }

    Status status = state.db.Verify();
    if (!status.ok()) {
        PrintStatus(status);
        return;
    }

    if (!deep) {
        std::cout << "OK\n";
        return;
    }

    // Beyond the structural check above, also cross-check that the sorted
    // scan order and each key's Get() result are mutually consistent.
    StatusOr<Database::Iterator> it_or = state.db.NewIterator();
    if (!it_or.ok()) {
        PrintStatus(it_or.status());
        return;
    }
    Database::Iterator iterator = std::move(it_or.value());

    std::string prev_key;
    bool have_prev = false;
    size_t count = 0;

    while (iterator.Valid()) {
        std::string key = iterator.Key().ToString();
        std::string value = iterator.Value().ToString();

        if (have_prev && !(prev_key < key)) {
            std::cout << "FAILED: scan order violated at key \"" << key << "\"\n";
            return;
        }

        StatusOr<std::string> direct = state.db.Get(Slice(key));
        if (!direct.ok() || direct.value() != value) {
            std::cout << "FAILED: Get(\"" << key << "\") does not match the scanned value\n";
            return;
        }

        prev_key = key;
        have_prev = true;
        ++count;

        Status next_status = iterator.Next();
        if (!next_status.ok()) {
            PrintStatus(next_status);
            return;
        }
    }

    std::cout << "OK (deep: " << count << " key(s) cross-checked)\n";
}

void ExecuteCheckpoint(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: checkpoint\n";
        return;
    }

    Status status = state.db.Checkpoint();
    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

void ExecuteInfo(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: info\n";
        return;
    }

    const Options& opts = state.db.options();
    std::cout << "Path:                " << opts.path << '\n'
              << "Page size:           " << opts.page_size << '\n'
              << "Buffer pool frames:  " << opts.buffer_pool_frames << '\n'
              << "Create if missing:   " << (opts.create_if_missing ? "yes" : "no") << '\n'
              << "Sync on commit:      " << (opts.sync_on_commit ? "yes" : "no") << '\n'
              << "Deadlock policy:     "
              << (opts.deadlock_policy == DeadlockPolicy::kDetection ? "detection" : "wound-wait")
              << '\n'
              << "Ran recovery:        " << (state.db.last_open_ran_recovery() ? "yes" : "no") << '\n';
}

void ExecuteStatus(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: status\n";
        return;
    }

    size_t open_snapshots = 0;
    for (const auto& snap : state.snapshots) {
        if (snap) {
            ++open_snapshots;
        }
    }

    std::cout << "Transaction active:  " << (TransactionIsActive(state) ? "yes" : "no") << '\n'
              << "Batch pending:       "
              << (state.batch ? std::to_string(state.batch->size()) + " op(s)" : "no") << '\n'
              << "Open snapshots:      " << open_snapshots << '\n';
}

void ExecuteBegin(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: begin\n";
        return;
    }
    if (TransactionIsActive(state)) {
        std::cout << "A transaction is already active. Use commit or rollback first.\n";
        return;
    }

    StatusOr<Transaction> txn_or = state.db.BeginTransaction();
    if (!txn_or.ok()) {
        PrintStatus(txn_or.status());
        return;
    }
    state.txn = std::move(txn_or.value());
    std::cout << "OK (transaction started)\n";
}

void ExecuteCommit(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: commit\n";
        return;
    }
    if (!TransactionIsActive(state)) {
        std::cout << "No active transaction.\n";
        return;
    }

    Status status = state.txn->Commit();
    if (status.ok()) {
        std::cout << "OK\n";
        state.txn.reset();
    } else {
        PrintStatus(status);
        std::cout << "Transaction is still active; retry commit or roll it back.\n";
    }
}

void ExecuteRollback(CliState& state, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: rollback\n";
        return;
    }
    if (!TransactionIsActive(state)) {
        std::cout << "No active transaction.\n";
        return;
    }

    Status status = state.txn->Rollback();
    if (status.ok()) {
        std::cout << "OK\n";
        state.txn.reset();
    } else {
        PrintStatus(status);
        std::cout << "Transaction is still active; retry rollback.\n";
    }
}

void ExecuteBatch(CliState& state, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Usage: batch <begin|put|delete|commit|cancel|status>\n";
        return;
    }
    const std::string& sub = args[1];

    if (sub == "begin") {
        if (args.size() != 2) {
            std::cout << "Usage: batch begin\n";
            return;
        }
        if (state.batch) {
            std::cout << "A write batch is already pending.\n";
            return;
        }
        state.batch = WriteBatch();
        std::cout << "OK (batch started)\n";
    } else if (sub == "put") {
        if (args.size() != 4) {
            std::cout << "Usage: batch put <key> <value>\n";
            return;
        }
        if (!state.batch) {
            std::cout << "No pending batch. Use 'batch begin' first.\n";
            return;
        }
        state.batch->Put(Slice(args[2]), Slice(args[3]));
        std::cout << "OK (" << state.batch->size() << " op(s) pending)\n";
    } else if (sub == "delete") {
        if (args.size() != 3) {
            std::cout << "Usage: batch delete <key>\n";
            return;
        }
        if (!state.batch) {
            std::cout << "No pending batch. Use 'batch begin' first.\n";
            return;
        }
        state.batch->Delete(Slice(args[2]));
        std::cout << "OK (" << state.batch->size() << " op(s) pending)\n";
    } else if (sub == "commit") {
        if (args.size() != 2) {
            std::cout << "Usage: batch commit\n";
            return;
        }
        if (!state.batch) {
            std::cout << "No pending batch.\n";
            return;
        }
        size_t op_count = state.batch->size();
        Status status = state.db.Write(*state.batch);
        if (status.ok()) {
            std::cout << "OK (" << op_count << " op(s) applied)\n";
            state.batch.reset();
        } else {
            PrintStatus(status);
            std::cout << "Batch is still pending (" << op_count << " op(s)); retry once resolved, "
                       "or 'batch cancel' to discard.\n";
        }
    } else if (sub == "cancel") {
        if (args.size() != 2) {
            std::cout << "Usage: batch cancel\n";
            return;
        }
        if (!state.batch) {
            std::cout << "No pending batch.\n";
            return;
        }
        std::cout << "OK (" << state.batch->size() << " op(s) discarded)\n";
        state.batch.reset();
    } else if (sub == "status") {
        if (args.size() != 2) {
            std::cout << "Usage: batch status\n";
            return;
        }
        if (state.batch) {
            std::cout << state.batch->size() << " op(s) pending\n";
        } else {
            std::cout << "No pending batch\n";
        }
    } else {
        std::cout << "Unknown batch subcommand: " << sub << '\n'
                  << "Usage: batch <begin|put|delete|commit|cancel|status>\n";
    }
}

void ExecuteSnapshot(CliState& state, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Usage: snapshot <create|get|list|close>\n";
        return;
    }
    const std::string& sub = args[1];

    if (sub == "create") {
        if (args.size() != 2) {
            std::cout << "Usage: snapshot create\n";
            return;
        }
        StatusOr<Snapshot> snap_or = state.db.CreateSnapshot();
        if (!snap_or.ok()) {
            PrintStatus(snap_or.status());
            return;
        }
        state.snapshots.push_back(std::move(snap_or.value()));
        std::cout << "OK (snapshot #" << state.snapshots.size() << ")\n";
    } else if (sub == "get") {
        if (args.size() != 4) {
            std::cout << "Usage: snapshot get <id> <key>\n";
            return;
        }
        size_t idx = 0;
        if (!ParseOpenSnapshotId(state, args[2], &idx)) {
            return;
        }
        StatusOr<std::string> result = state.snapshots[idx]->Get(Slice(args[3]));
        if (!result.ok()) {
            PrintStatus(result.status());
            return;
        }
        std::cout << result.value() << '\n';
    } else if (sub == "list") {
        if (args.size() != 2) {
            std::cout << "Usage: snapshot list\n";
            return;
        }
        bool any = false;
        for (size_t i = 0; i < state.snapshots.size(); ++i) {
            if (state.snapshots[i]) {
                std::cout << "#" << (i + 1) << '\n';
                any = true;
            }
        }
        if (!any) {
            std::cout << "No open snapshots\n";
        }
    } else if (sub == "close") {
        if (args.size() != 3) {
            std::cout << "Usage: snapshot close <id>\n";
            return;
        }
        size_t idx = 0;
        if (!ParseOpenSnapshotId(state, args[2], &idx)) {
            return;
        }
        state.snapshots[idx].reset();
        std::cout << "OK (snapshot closed)\n";
    } else {
        std::cout << "Unknown snapshot subcommand: " << sub << '\n'
                  << "Usage: snapshot <create|get|list|close>\n";
    }
}

bool ExecuteCommand(CliState& state, const std::vector<std::string>& args) {
    if (args.empty()) {
        return true;
    }

    const std::string& command = args[0];

    if (command == "put") {
        ExecutePut(state, args);
    } else if (command == "get") {
        ExecuteGet(state, args);
    } else if (command == "delete" || command == "remove") {
        ExecuteDelete(state, args);
    } else if (command == "scan") {
        ExecuteScan(state, args);
    } else if (command == "stats") {
        ExecuteStats(state, args);
    } else if (command == "verify") {
        ExecuteVerify(state, args);
    } else if (command == "checkpoint") {
        ExecuteCheckpoint(state, args);
    } else if (command == "info") {
        ExecuteInfo(state, args);
    } else if (command == "status") {
        ExecuteStatus(state, args);
    } else if (command == "begin") {
        ExecuteBegin(state, args);
    } else if (command == "commit") {
        ExecuteCommit(state, args);
    } else if (command == "rollback") {
        ExecuteRollback(state, args);
    } else if (command == "batch") {
        ExecuteBatch(state, args);
    } else if (command == "snapshot") {
        ExecuteSnapshot(state, args);
    } else if (command == "help") {
        if (args.size() != 1) {
            std::cout << "Usage: help\n";
        } else {
            PrintHelp();
        }
    } else if (command == "exit" || command == "quit") {
        if (args.size() != 1) {
            std::cout << "Usage: exit\n";
        } else {
            return false;
        }
    } else {
        std::cout << "Unknown command: " << command << '\n'
                  << "Type 'help' for available commands.\n";
    }

    return true;
}

} // namespace
} // namespace engine

int main(int argc, char* argv[]) {
    using namespace engine;

    Options options;
    std::string db_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--page-size" && i + 1 < argc) {
            options.page_size = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--buffer-pool-frames" && i + 1 < argc) {
            options.buffer_pool_frames = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (arg == "--no-create") {
            options.create_if_missing = false;
        } else if (arg == "--no-sync") {
            options.sync_on_commit = false;
        } else if (arg == "--deadlock-detection") {
            options.deadlock_policy = DeadlockPolicy::kDetection;
        } else if (arg == "--detection-interval-ms" && i + 1 < argc) {
            options.deadlock_detection_interval = std::chrono::milliseconds(std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown flag: " << arg << "\n\n";
            PrintUsage();
            return 1;
        } else if (db_path.empty()) {
            db_path = arg;
        } else {
            std::cerr << "Unexpected argument: " << arg << "\n\n";
            PrintUsage();
            return 1;
        }
    }

    if (db_path.empty()) {
        PrintUsage();
        return 1;
    }
    options.path = db_path;

    Database db(options);

    Status status = db.Open();
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << '\n';
        return 1;
    }

    std::cout << "Yggdrasil database opened: " << options.path << '\n';
    if (db.last_open_ran_recovery()) {
        std::cout << "(recovered from a non-empty WAL)\n";
    }
    std::cout << "Type 'help' for available commands.\n";

    CliState state(db);

    while (true) {
        std::cout << "yggdrasil> " << std::flush;

        std::string line;

        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            break;
        }

        auto args = Tokenize(line);

        if (!ExecuteCommand(state, args)) {
            break;
        }
    }

    if (state.txn && state.txn->is_active()) {
        std::cout << "Rolling back active transaction...\n";
        state.txn->Rollback();
    }
    if (state.batch) {
        std::cout << "Discarding pending batch (" << state.batch->size() << " op(s))...\n";
        state.batch.reset();
    }
    state.snapshots.clear();

    status = db.Close();

    if (!status.ok()) {
        std::cerr << "Failed to close database: " << status.ToString() << '\n';
        return 1;
    }

    return 0;
}

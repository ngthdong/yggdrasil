#include "engine/database.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace {

void PrintHelp() {
    std::cout
        << "Commands:\n"
        << "  put <key> <value>  Insert or update a key\n"
        << "  get <key>          Get a value\n"
        << "  delete <key>       Delete a key\n"
        << "  scan               Scan all keys in sorted order\n"
        << "  stats              Show database statistics\n"
        << "  verify             Verify database invariants\n"
        << "  help               Show this help\n"
        << "  exit               Exit the CLI\n";
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

void ExecutePut(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 3) {
        std::cout << "Usage: put <key> <value>\n";
        return;
    }

    Status status = db.Put(Slice(args[1]), Slice(args[2]));

    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

void ExecuteGet(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "Usage: get <key>\n";
        return;
    }

    StatusOr<std::string> result = db.Get(Slice(args[1]));

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    std::cout << result.value() << '\n';
}

void ExecuteDelete(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "Usage: delete <key>\n";
        return;
    }

    Status status = db.Remove(Slice(args[1]));

    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

void ExecuteScan(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: scan\n";
        return;
    }

    StatusOr<Database::Iterator> result = db.NewIterator();

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    Database::Iterator iterator = std::move(result.value());

    while (iterator.Valid()) {
        std::cout << iterator.Key().ToString()
                  << " = "
                  << iterator.Value().ToString()
                  << '\n';

        Status status = iterator.Next();

        if (!status.ok()) {
            PrintStatus(status);
            return;
        }
    }
}

void ExecuteStats(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: stats\n";
        return;
    }

    StatusOr<DBStats> result = db.GetStats();

    if (!result.ok()) {
        PrintStatus(result.status());
        return;
    }

    const DBStats& stats = result.value();

    std::cout << "Pages:              " << stats.page_count << '\n'
              << "Buffer pool:        "
              << stats.buffer_pool_capacity_frames << '\n'
              << "Resident frames:    "
              << stats.buffer_pool_resident_frames << '\n'
              << "Buffer pool hit:    "
              << stats.BufferPoolHitRate() * 100.0 << "%\n"
              << "Tree height:        "
              << stats.tree_height << '\n';
}

void ExecuteVerify(Database& db, const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "Usage: verify\n";
        return;
    }

    Status status = db.Verify();

    if (status.ok()) {
        std::cout << "OK\n";
    } else {
        PrintStatus(status);
    }
}

bool ExecuteCommand(Database& db, const std::vector<std::string>& args) {
    if (args.empty()) {
        return true;
    }

    const std::string& command = args[0];

    if (command == "put") {
        ExecutePut(db, args);
    } else if (command == "get") {
        ExecuteGet(db, args);
    } else if (command == "delete") {
        ExecuteDelete(db, args);
    } else if (command == "scan") {
        ExecuteScan(db, args);
    } else if (command == "stats") {
        ExecuteStats(db, args);
    } else if (command == "verify") {
        ExecuteVerify(db, args);
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

}  // namespace
}  // namespace engine

int main(int argc, char* argv[]) {
    using namespace engine;

    if (argc != 2) {
        std::cerr << "Usage: yggdrasil-cli <database>\n";
        return 1;
    }

    Options options;
    options.path = argv[1];

    Database db(options);

    Status status = db.Open();
    if (!status.ok()) {
        std::cerr << "Failed to open database: "
                  << status.ToString() << '\n';
        return 1;
    }

    std::cout << "Yggdrasil database opened: " << options.path << '\n';
    std::cout << "Type 'help' for available commands.\n";

    while (true) {
        std::cout << "yggdrasil> " << std::flush;

        std::string line;

        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            break;
        }

        auto args = Tokenize(line);

        if (!ExecuteCommand(db, args)) {
            break;
        }
    }

    status = db.Close();

    if (!status.ok()) {
        std::cerr << "Failed to close database: "
                  << status.ToString() << '\n';
        return 1;
    }

    return 0;
}
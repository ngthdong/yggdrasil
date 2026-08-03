#include <iostream>

#include "engine/disk_manager.h"

int main() {
    auto dm = engine::DiskManager::Open(
        "test.db",
        4096,
        /*create_if_missing=*/true);

    if (!dm.ok()) {
        std::cerr << dm.status().ToString() << '\n';
        return 1;
    }

    std::cout << "Database created.\n";
}
// wal_dump: parses and prints every record in a WAL file, in order, the
// same way RecoveryManager does. Usage: wal_dump <file.wal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "engine/log_record.h"

namespace {

const char* TypeName(engine::LogRecordType type) {
    switch (type) {
        case engine::LogRecordType::kInvalid:
            return "INVALID";
        case engine::LogRecordType::kInsert:
            return "INSERT";
        case engine::LogRecordType::kDelete:
            return "DELETE";
        case engine::LogRecordType::kBegin:
            return "BEGIN";
        case engine::LogRecordType::kCommit:
            return "COMMIT";
        case engine::LogRecordType::kAbort:
            return "ABORT";
        case engine::LogRecordType::kCheckpointBegin:
            return "CHECKPOINT_BEGIN";
        case engine::LogRecordType::kCheckpointEnd:
            return "CHECKPOINT_END";
    }
    return "UNKNOWN";
}

// Renders a string for terminal display, escaping bytes outside printable
// ASCII so binary keys/values don't corrupt the output.
std::string Printable(const std::string& s) {
    std::ostringstream out;
    for (char raw_c : s) {
        unsigned char c = static_cast<unsigned char>(raw_c);
        if (c >= 32 && c < 127) {
            out << static_cast<char>(c);
        } else {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out << buf;
        }
    }
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: wal_dump <file.wal>\n";
        return 1;
    }
    std::string path = argv[1];

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "failed to open " << path << "\n";
        return 1;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buf(static_cast<size_t>(size));
    if (size > 0 && !file.read(buf.data(), size)) {
        std::cerr << "failed to read " << path << "\n";
        return 1;
    }

    std::printf("wal file %s: %lld bytes\n", path.c_str(), static_cast<long long>(size));

    size_t offset = 0;
    int count = 0;
    while (offset < buf.size()) {
        size_t consumed = 0;
        auto rec_or =
            engine::LogRecord::ParseFrom(buf.data(), buf.size(), offset, &consumed);
        if (!rec_or.ok()) {
            std::printf("stopped at offset %zu (%zu trailing bytes unparsed): %s\n",
                       offset,
                       buf.size() - offset,
                       rec_or.status().ToString().c_str());
            break;
        }

        const engine::LogRecord& rec = rec_or.value();
        std::printf("[%d] offset=%-6zu lsn=%-6llu txn=%-6llu %-16s page_id=%-4d key=\"%s\" value=\"%s\"\n",
                   count,
                   offset,
                   static_cast<unsigned long long>(rec.lsn),
                   static_cast<unsigned long long>(rec.txn_id),
                   TypeName(rec.type),
                   rec.page_id,
                   Printable(rec.key).c_str(),
                   Printable(rec.value).c_str());

        offset += consumed;
        ++count;
    }

    std::printf("%d record(s), %zu byte(s) parsed\n", count, offset);
    return 0;
}

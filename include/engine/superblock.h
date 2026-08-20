#pragma once

#include <cstdint>
#include <cstring>

#include "engine/byte_utils.h"
#include "engine/config.h"
#include "engine/status.h"

namespace engine {

// The superblock lives at page 0 and is the one page whose layout the
// DiskManager itself understands (every other page is opaque bytes to it).
struct Superblock {
    uint32_t format_version = kFormatVersion;
    uint32_t page_size = 4096;
    uint32_t page_count = 1;
    int32_t free_list_head_page_id = kInvalidPageId;
    int32_t root_page_id = kInvalidPageId;
    uint64_t last_checkpoint_lsn = kInvalidLsn;

    void SerializeTo(char* buf, uint32_t buf_size) const {
        std::memset(buf, 0, buf_size);
        size_t off = 0;
        std::memcpy(buf + off, kSuperblockMagic, 4);
        off += 4;
        PutU32(buf + off, format_version);
        off += 4;
        PutU32(buf + off, page_size);
        off += 4;
        PutU32(buf + off, page_count);
        off += 4;
        PutI32(buf + off, free_list_head_page_id);
        off += 4;
        PutI32(buf + off, root_page_id);
        off += 4;
        PutU64(buf + off, last_checkpoint_lsn);
        off += 8;
    }

    static StatusOr<Superblock> DeserializeFrom(const char* buf, uint32_t buf_size) {
        if (buf_size < 24) {
            return Status ::Corruption("superblock page too small to contain a header");
        }
        if (std::memcmp(buf, kSuperblockMagic, 4) != 0) {
            return Status::Corruption("superblock magic number mismatch: not an engine file");
        }
        Superblock sb;
        size_t off = 4;
        sb.format_version = GetU32(buf + off);
        off += 4;
        sb.page_size = GetU32(buf + off);
        off += 4;
        sb.page_count = GetU32(buf + off);
        off += 4;
        sb.free_list_head_page_id = GetI32(buf + off);
        off += 4;
        sb.root_page_id = GetI32(buf + off);
        off += 4;
        sb.last_checkpoint_lsn = GetU64(buf + off);
        off += 8;
        if (sb.format_version != kFormatVersion) {
            return Status::Corruption("unsupported format_version " +
                                      std::to_string(sb.format_version));
        }
        return sb;
    }
};

} // namespace engine
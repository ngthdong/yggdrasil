#pragma once

#include <cstdint>
#include <cstring>

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
        if (sb.format_version != kFormatVersion) {
            return Status::Corruption("unsupported format_version " +
                                      std::to_string(sb.format_version));
        }
        return sb;
    }

  private:
    // Explicit little-endian encode/decode to avoid any issues with host endianness.
    static void PutU32(char* p, uint32_t v) {
        p[0] = static_cast<char>(v & 0xFF);
        p[1] = static_cast<char>((v >> 8) & 0xFF);
        p[2] = static_cast<char>((v >> 16) & 0xFF);
        p[3] = static_cast<char>((v >> 24) & 0xFF);
    }

    static void PutI32(char* p, int32_t v) { PutU32(p, static_cast<uint32_t>(v)); }

    static uint32_t GetU32(const char* p) {
        return (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))) |
               (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
    }

    static int32_t GetI32(const char* p) { return static_cast<int32_t>(GetU32(p)); }
};

} // namespace engine
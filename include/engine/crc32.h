#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

// Computes a simple bit-by-bit CRC32 checksum using zlib's polynomial
// (0xEDB88320). This implementation prioritizes correctness and simplicity
// over performance; WAL records are small, so a table-based or
// hardware-accelerated implementation is unnecessary.
inline uint32_t Crc32(const char* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (~(crc & 1u)) + 1u; // 0xFFFFFFFF if crc&1 set, else 0
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

} // namespace engine
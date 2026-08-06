#pragma once

#include <cstdint>

namespace engine {
inline void PutU16(char* p, uint16_t v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
}

inline uint16_t GetU16(const char* p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[1]) << 8) | static_cast<uint8_t>(p[0]));
}

static void PutU32(char* p, uint32_t v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
    p[2] = static_cast<char>((v >> 16) & 0xFF);
    p[3] = static_cast<char>((v >> 24) & 0xFF);
}

static void PutI32(char* p, int32_t v) {
    PutU32(p, static_cast<uint32_t>(v));
}

static uint32_t GetU32(const char* p) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

static int32_t GetI32(const char* p) {
    return static_cast<int32_t>(GetU32(p));
}

} // namespace engine
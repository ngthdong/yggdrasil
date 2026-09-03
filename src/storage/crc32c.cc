#include "engine/crc32c.h"
#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_CRC32C_X86 1
#include <cpuid.h>
#endif

namespace engine {

namespace {

// Reflected CRC32C (Castagnoli) polynomial.
constexpr uint32_t kCrc32cPoly = 0x82F63B78u;

std::array<uint32_t, 256> BuildTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (~(crc & 1u)) + 1u;
            crc = (crc >> 1) ^ (kCrc32cPoly & mask);
        }
        table.at(i) = crc;
    }
    return table;
}

const std::array<uint32_t, 256>& Table() {
    static const std::array<uint32_t, 256> table = BuildTable();
    return table;
}

#if defined(ENGINE_CRC32C_X86)
bool DetectSse42() {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (ecx & (1u << 20)) != 0; // SSE4.2 feature bit
}

// Compile this function with SSE4.2 enabled without requiring SSE4.2
// support for the rest of the translation unit. The function is called
// only after runtime CPU-feature detection confirms SSE4.2 support.
__attribute__((target("sse4.2"))) uint32_t Crc32cHardware(const char* data, size_t len) {
    uint64_t crc = 0xFFFFFFFFu;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);

    // Use memcpy to safely load unaligned data. The input buffer is not
    // required to satisfy the alignment requirements of integer types.
    uint64_t v64;
    while (len >= 8) {
        std::memcpy(&v64, p, sizeof(v64));
        crc = __builtin_ia32_crc32di(crc, v64);
        p += 8;
        len -= 8;
    }

    uint32_t v32;
    while (len >= 4) {
        std::memcpy(&v32, p, 4);
        crc = __builtin_ia32_crc32si(static_cast<uint32_t>(crc), v32);
        p += 4;
        len -= 4;
    }

    while (len > 0) {
        crc = __builtin_ia32_crc32qi(static_cast<uint32_t>(crc), *p);
        ++p;
        --len;
    }

    return static_cast<uint32_t>(crc) ^ 0xFFFFFFFFu;
}
#endif

} // namespace

uint32_t Crc32cSoftware(const char* data, size_t len) {
    const auto& table = Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table.at((crc ^ static_cast<uint8_t>(data[i])) & 0xFFu) ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool Crc32cUsesHardware() {
#if defined(ENGINE_CRC32C_X86)
    static const bool has_sse42 = DetectSse42();
    return has_sse42;
#else
    return false;
#endif
}

uint32_t Crc32c(const char* data, size_t len) {
#if defined(ENGINE_CRC32C_X86)
    if (Crc32cUsesHardware()) {
        return Crc32cHardware(data, len);
    }
#endif
    return Crc32cSoftware(data, len);
}

} // namespace engine
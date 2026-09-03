#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

// Computes the CRC32C checksum of the given byte sequence.
// The implementation selects the fatest available execution path,
// using hardware acceleration when supported and a table-based
// software implementation otherwise.
uint32_t Crc32c(const char* data, size_t len);

// Returns whether the hardware-accelerated CRC32C path is available.
// Intended primarily for testing and diagnostics.
bool Crc32cUsesHardware();

// Computes CRC32C using the portable table-based implementation.
// This path is always available regradless of CPU capacitilies and is
// uses as the fallback implementation.
uint32_t Crc32cSoftware(const char* data, size_t len);

} // namespace engine

#pragma once

#include <cstdint>

namespace engine {

using page_id_t = int32_t;
using frame_id_t = int32_t;

constexpr page_id_t kInvalidPageId = -1;
constexpr page_id_t kSuperblockPageId = 0;
constexpr frame_id_t kInvalidFrameId = -1;

enum class PageType : uint8_t {
    kInvalid = 0,
    kSuperblock = 1,
    kFreeListNode = 2,
    kBTreeLeaf = 3,
    kBTreeInternal = 4,
};

// Superblock magic types, written as raw ASCII so `xxd`/`page_dump` output
// is human-readable even before any decoding logic runs.
constexpr char kSuperblockMagic[4] = {'E', 'N', 'G', 'N'};
constexpr uint32_t kFormatVersion = 1;

} // namespace engine
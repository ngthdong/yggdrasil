#pragma once

#include <cstdint>

namespace engine {
using page_id_t = int32_t;

constexpr page_id_t kInvalidPageId = -1;
constexpr page_id_t kSuperblockPageId = 0;

constexpr char kSuperblockMagic[4] = {'E', 'N', 'G', 'N'};
constexpr uint32_t kFormatVersion = 1;

} // namespace engine
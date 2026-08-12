#pragma once

#include <cstring>

#include "engine/byte_utils.h"
#include "engine/config.h"
#include "engine/status.h"

namespace engine {

// [0:1) page_type (u8), must be PageType::kFreeListNode
// [1:5) next_free_page_id (i32, kInvalidPageId == end of list)
struct FreeListNode {
    page_id_t next_free_page_id = kInvalidPageId;

    static void SerializeTo(char* buf, uint32_t page_size, page_id_t next_free_page_id) {
        std::memset(buf, 0, page_size);
        buf[0] = static_cast<char>(static_cast<uint8_t>(PageType::kFreeListNode));
        PutI32(buf + 1, next_free_page_id);
    }

    static StatusOr<FreeListNode> DeserializeFrom(const char* buf, uint32_t page_size) {
        if (page_size < 5)
            return Status::Corruption("page too small for a free-list node header");
        auto type = static_cast<PageType>(static_cast<uint8_t>(buf[0]));
        if (type != PageType::kFreeListNode) {
            return Status::Corruption(
                "free-list chain points at a page that is not a free-list node "
                "(page_type byte mismatch); possible free-list corruption");
        }
        FreeListNode node;
        node.next_free_page_id = GetI32(buf + 1);
        return node;
    }
};

} // namespace engine
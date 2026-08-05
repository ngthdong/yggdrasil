#pragma once

#include <cstdint>
#include <string>

#include "engine/config.h"
#include "engine/slice.h"
#include "engine/status.h"

namespace engine {
using slot_id_t = uint16_t;

// SlottedPage is a non-owning view over a fixed-size page buffer. It never
// allocates or owns memory; the caller manages the buffer lifetime.
//
// On-disk layout (little-endian):
//
//   Header (12 bytes)
//     [0:4)   page_id
//     [4:6)   num_slots
//     [6:8)   free_space_offset
//     [8:12)  checksum (reserved)
//
//   Slot directory
//     [12:...) num_slots entries:
//                offset (u16)
//                length (u16)
//                flags  (u8, bit0 = tombstone)
//
//   Record area
//     [free_space_offset, page_size)
//
// Slot IDs are stable: deleted slots are tombstoned in place rather than
// removed or reused.
class SlottedPage {
  public:
    static constexpr uint32_t kHeaderSize = 12;
    static constexpr uint32_t kSlotEntrySize = 5;
    static constexpr uint8_t kTombstoneFlag = 0x1;
    static constexpr slot_id_t kInvalidSlotId = 0xFFFF;

    static void InitNewPage(char* buf, uint32_t page_size, page_id_t page_id);

    SlottedPage(char* buf, uint32_t page_size) : buf_(buf), page_size_(page_size) {}

    page_id_t page_id() const;
    uint16_t num_slots() const;
    uint16_t free_space_offset() const;

    uint32_t FreeSpaceContiguous() const;

    uint32_t FreeSpaceReclaimable() const;

    StatusOr<slot_id_t> InsertRecord(Slice record);

    StatusOr<Slice> GetRecord(slot_id_t slot_id) const;

    Status DeleteRecord(slot_id_t slot_id);

    void Compact();

  private:
    uint32_t SlotOffset(slot_id_t slot_id) const { return kHeaderSize + slot_id * kSlotEntrySize; }
    uint32_t SlotDirectoryEnd() const { return kHeaderSize + num_slots() * kSlotEntrySize; }

    void set_num_slots(uint16_t n);
    void set_free_space_offset(uint16_t off);

    char* buf_;
    uint32_t page_size_;
};

} // namespace engine
#include "engine/slotted_page.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/byte_utils.h"

namespace engine {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SlottedPage::InitNewPage(char* buf, uint32_t page_size, page_id_t page_id) {
    PutI32(buf + 0, page_id);
    PutU16(buf + 4, 0);
    PutU16(buf + 6, static_cast<uint16_t>(page_size));
    PutU32(buf + 8, 0);
}

page_id_t SlottedPage::page_id() const { return GetI32(buf_ + 0); }
uint16_t SlottedPage::num_slots() const { return GetU16(buf_ + 4); }
uint16_t SlottedPage::free_space_offset() const { return GetU16(buf_ + 6); }

void SlottedPage::set_num_slots(uint16_t n) { PutU16(buf_ + 4, n); }
void SlottedPage::set_free_space_offset(uint16_t off) { PutU16(buf_ + 6, off); }

uint32_t SlottedPage::FreeSpaceContiguous() const {
    uint32_t dir_end = SlotDirectoryEnd();
    uint32_t data_start = free_space_offset();
    if (dir_end > data_start) {
        return 0;
    }
    return data_start - dir_end;
}

uint32_t SlottedPage::FreeSpaceReclaimable() const {
    uint16_t n = num_slots();
    uint32_t live_bytes = 0;
    for (slot_id_t i = 0; i < n; ++i) {
        uint32_t slot_off = SlotOffset(i);
        uint8_t flags = static_cast<uint8_t>(buf_[slot_off + 4]);
        if ((flags & kTombstoneFlag) != 0) {
            continue;
        }
        live_bytes += GetU16(buf_ + slot_off + 2);
    }
    uint32_t dir_end = SlotDirectoryEnd();
    if (dir_end + live_bytes > page_size_) {
        return 0;
    }
    return page_size_ - dir_end - live_bytes;
}

StatusOr<slot_id_t> SlottedPage::InsertRecord(Slice record) {
    if (record.empty()) {
        return Status::InvalidArgument("SlottedPage::InsertRecord: cannot insert an empty record");
    }
    uint32_t needed = static_cast<uint32_t>(record.size()) + kSlotEntrySize;
    if (needed > page_size_) {
        return Status::InvalidArgument("SlottedPage::InsertRecord: record larger than page_size");
    }

    if (FreeSpaceContiguous() < needed) {
        if (FreeSpaceReclaimable() < needed) {
            return Status::ResourceExhausted(
                "SlottedPage::InsertRecord: page full (not enough space even after compaction)");
        }
        Compact();
    }

    uint16_t n = num_slots();
    uint16_t new_free_offset = static_cast<uint16_t>(free_space_offset() - record.size());
    std::memcpy(buf_ + new_free_offset, record.data(), record.size());

    uint32_t slot_off = SlotOffset(n);
    PutU16(buf_ + slot_off, new_free_offset);
    PutU16(buf_ + slot_off + 2, static_cast<uint16_t>(record.size()));
    buf_[slot_off + 4] = 0;

    set_num_slots(static_cast<uint16_t>(n + 1));
    set_free_space_offset(new_free_offset);
    return static_cast<slot_id_t>(n);
}

StatusOr<Slice> SlottedPage::GetRecord(slot_id_t slot_id) const {
    uint16_t n = num_slots();
    if (slot_id >= n) {
        return Status::NotFound("SlottedPage::GetRecord: slot_id " + std::to_string(slot_id) +
                                " out of range (num_slots=" + std::to_string(n) + ")");
    }
    uint32_t slot_off = SlotOffset(slot_id);
    uint16_t offset = GetU16(buf_ + slot_off);
    uint16_t length = GetU16(buf_ + slot_off + 2);
    uint8_t flags = static_cast<uint8_t>(buf_[slot_off + 4]);

    if ((flags & kTombstoneFlag) != 0) {
        return Status::NotFound("SlottedPage::GetRecord: slot_id " + std::to_string(slot_id) +
                                " was deleted");
    }

    uint64_t end = static_cast<uint64_t>(offset) + length;
    uint32_t dir_end = SlotDirectoryEnd();
    if (end > page_size_ || offset < dir_end) {
        return Status::Corruption("SlottedPage::GetRecord: slot " + std::to_string(slot_id) +
                                  " has an out-of-bounds or overlapping offset/length "
                                  "(offset=" +
                                  std::to_string(offset) + ", length=" + std::to_string(length) +
                                  ")");
    }
    return Slice(buf_ + offset, length);
}

Status SlottedPage::DeleteRecord(slot_id_t slot_id) {
    uint16_t n = num_slots();
    if (slot_id >= n) {
        return Status::NotFound("SlottedPage::DeleteRecord: slot_id " + std::to_string(slot_id) +
                                " out of range (num_slots=" + std::to_string(n) + ")");
    }
    uint32_t slot_off = SlotOffset(slot_id);
    uint8_t flags = static_cast<uint8_t>(buf_[slot_off + 4]);
    if ((flags & kTombstoneFlag) != 0) {
        return Status::NotFound("SlottedPage::DeleteRecord: slot_id " + std::to_string(slot_id) +
                                " already deleted");
    }
    buf_[slot_off + 4] = static_cast<char>(flags | kTombstoneFlag);
    return Status::OK();
}

void SlottedPage::Compact() {
    uint16_t n = num_slots();
    // Copy through a separate scratch buffer rather than shuffling in place.
    // avoiding overlapping-memmove hazards when packing records that may
    // already be adjacent to their new location.
    std::vector<char> scratch(page_size_, 0);
    uint32_t write_cursor = page_size_;

    for (slot_id_t i = 0; i < n; ++i) {
        uint32_t slot_off = SlotOffset(i);
        uint8_t flags = static_cast<uint8_t>(buf_[slot_off + 4]);
        if ((flags & kTombstoneFlag) != 0) {
            continue;
        }

        uint16_t old_offset = GetU16(buf_ + slot_off);
        uint16_t length = GetU16(buf_ + slot_off + 2);

        write_cursor -= length;
        std::memcpy(scratch.data() + write_cursor, buf_ + old_offset, length);
        PutU16(buf_ + slot_off, static_cast<uint16_t>(write_cursor)); // slot_id/flags untouched
    }

    std::memcpy(buf_ + write_cursor, scratch.data() + write_cursor, page_size_ - write_cursor);
    set_free_space_offset(static_cast<uint16_t>(write_cursor));
}

} // namespace engine
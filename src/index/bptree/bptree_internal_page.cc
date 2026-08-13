#include "engine/bptree_internal_page.h"

#include <cstring>

#include "engine/byte_utils.h"

namespace engine {

void BPlusTreeInternalPage::InitNewPage(char* buf,
                                        uint32_t page_size,
                                        page_id_t page_id,
                                        page_id_t leftmost_child_id) {
    PutI32(buf + 0, page_id);
    buf[4] = static_cast<char>(static_cast<uint8_t>(PageType::kBTreeInternal));
    PutU16(buf + 5, 0);
    PutU16(buf + 7, static_cast<uint16_t>(page_size));
    PutI32(buf + 9, leftmost_child_id);
    PutU32(buf + 13, 0);
}

page_id_t BPlusTreeInternalPage::page_id() const {
    return GetI32(buf_ + 0);
}
uint16_t BPlusTreeInternalPage::num_keys() const {
    return GetU16(buf_ + 5);
}
uint16_t BPlusTreeInternalPage::free_space_offset() const {
    return GetU16(buf_ + 7);
}

void BPlusTreeInternalPage::set_num_keys(uint16_t n) {
    PutU16(buf_ + 5, n);
}
void BPlusTreeInternalPage::set_free_space_offset(uint16_t off) {
    PutU16(buf_ + 7, off);
}
void BPlusTreeInternalPage::set_leftmost_child_id(page_id_t id) {
    PutI32(buf_ + 9, id);
}

uint32_t BPlusTreeInternalPage::FreeSpaceContiguous() const {
    uint32_t dir_end = DirEnd();
    uint32_t data_start = free_space_offset();
    if (dir_end > data_start) {
        return 0;
    }
    return data_start - dir_end;
}

Slice BPlusTreeInternalPage::KeyAt(uint16_t index) const {
    uint16_t offset = GetU16(buf_ + EntryOffset(index));
    uint16_t key_len = GetU16(buf_ + offset);
    return {buf_ + offset + 2, key_len};
}

page_id_t BPlusTreeInternalPage::ChildAt(uint16_t index) const {
    if (index == 0) {
        return GetI32(buf_ + 9);
    }
    uint32_t entry_off = EntryOffset(static_cast<uint16_t>(index - 1));
    return GetI32(buf_ + entry_off + 2);
}

uint16_t BPlusTreeInternalPage::FindChildIndex(const Slice& key, const KeyComparator& cmp) const {
    uint16_t lo = 0;
    uint16_t hi = num_keys();
    while (lo < hi) {
        uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (cmp(KeyAt(mid), key) <= 0) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

Status BPlusTreeInternalPage::InsertEntry(uint16_t index, const Slice& key, page_id_t child_id) {
    uint32_t record_size = 2 + static_cast<uint32_t>(key.size());
    uint32_t needed = record_size + kEntrySize;
    if (needed > FreeSpaceContiguous()) {
        Compact();
        return Status::ResourceExhausted(
            "BPlusTreeInternalPage::InsertEntry: internal page full, caller must split");
    }

    uint16_t new_offset = static_cast<uint16_t>(free_space_offset() - record_size);
    PutU16(buf_ + new_offset, static_cast<uint16_t>(key.size()));
    std::memcpy(buf_ + new_offset + 2, key.data(), key.size());

    uint16_t n = num_keys();
    for (uint16_t i = n; i > index; --i) {
        uint32_t src = EntryOffset(static_cast<uint16_t>(i - 1));
        uint32_t dst = EntryOffset(i);
        uint16_t off = GetU16(buf_ + src);
        page_id_t cid = GetI32(buf_ + src + 2);
        PutU16(buf_ + dst, off);
        PutI32(buf_ + dst + 2, cid);
    }
    uint32_t entry_off = EntryOffset(index);
    PutU16(buf_ + entry_off, new_offset);
    PutI32(buf_ + entry_off + 2, child_id);

    set_num_keys(static_cast<uint16_t>(n + 1));
    set_free_space_offset(new_offset);
    return Status::OK();
}

Status BPlusTreeInternalPage::RemoveEntry(uint16_t index) {
    uint16_t n = num_keys();
    for (uint16_t i = index; static_cast<uint16_t>(i + 1) < n; ++i) {
        uint32_t src = EntryOffset(static_cast<uint16_t>(i + 1));
        uint32_t dst = EntryOffset(i);
        uint16_t off = GetU16(buf_ + src);
        page_id_t cid = GetI32(buf_ + src + 2);
        PutU16(buf_ + dst, off);
        PutI32(buf_ + dst + 2, cid);
    }
    set_num_keys(static_cast<uint16_t>(n - 1));
    return Status::OK();
}

Status BPlusTreeInternalPage::UpdateKeyAt(uint16_t index, const Slice& new_key) {
    uint32_t needed = 2 + static_cast<uint32_t>(new_key.size());
    if (needed > FreeSpaceContiguous()) {
        Compact();
        if (needed > FreeSpaceContiguous()) {
            return Status::ResourceExhausted(
                "BPlusTreeInternalPage::UpdateKeyAt: no room for the new key's "
                "record even after compaction");
        }
    }
    uint16_t new_offset = static_cast<uint16_t>(free_space_offset() - needed);
    PutU16(buf_ + new_offset, static_cast<uint16_t>(new_key.size()));
    std::memcpy(buf_ + new_offset + 2, new_key.data(), new_key.size());

    uint32_t entry_off = EntryOffset(index);
    PutU16(buf_ + entry_off, new_offset); // repoint the directory entry only
    set_free_space_offset(new_offset);
    return Status::OK();
}

void BPlusTreeInternalPage::Compact() {
    uint16_t n = num_keys();
    page_id_t saved_leftmost = ChildAt(0);
    std::vector<std::pair<std::string, page_id_t>> entries;
    entries.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        entries.emplace_back(KeyAt(i).ToString(), ChildAt(static_cast<uint16_t>(i + 1)));
    }
    page_id_t saved_id = page_id();

    InitNewPage(buf_, page_size_, saved_id, saved_leftmost);
    for (const auto& [k, child] : entries) {
        AppendEntryUnchecked(Slice(k), child);
    }
}

void BPlusTreeInternalPage::AppendEntryUnchecked(const Slice& key, page_id_t child_id) {
    uint32_t record_size = 2 + static_cast<uint32_t>(key.size());
    uint16_t new_offset = static_cast<uint16_t>(free_space_offset() - record_size);
    PutU16(buf_ + new_offset, static_cast<uint16_t>(key.size()));
    std::memcpy(buf_ + new_offset + 2, key.data(), key.size());

    uint16_t n = num_keys();
    uint32_t entry_off = EntryOffset(n); // always at the end, no shift needed
    PutU16(buf_ + entry_off, new_offset);
    PutI32(buf_ + entry_off + 2, child_id);

    set_num_keys(static_cast<uint16_t>(n + 1));
    set_free_space_offset(new_offset);
}

} // namespace engine
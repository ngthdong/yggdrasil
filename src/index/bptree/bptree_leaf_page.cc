#include <cstring>

#include "engine/bptree_leaf_page.h"
#include "engine/byte_utils.h"
#include "engine/config.h"

namespace engine {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void BPlusTreeLeafPage::InitNewPage(char* buf, uint32_t page_size, page_id_t page_id) {
    PutI32(buf + 0, page_id);
    buf[4] = static_cast<char>(static_cast<uint8_t>(PageType::kBTreeLeaf));
    PutU16(buf + 5, 0);                                // num_keys
    PutU16(buf + 7, static_cast<uint16_t>(page_size)); // free_space_offset
    PutI32(buf + 9, kInvalidPageId);                   // next_leaf_page_id
    PutU32(buf + 13, 0);                               // checksum placeholder
}

page_id_t BPlusTreeLeafPage::page_id() const {
    return GetI32(buf_ + 0);
}
uint16_t BPlusTreeLeafPage::num_keys() const {
    return GetU16(buf_ + 5);
}
uint16_t BPlusTreeLeafPage::free_space_offset() const {
    return GetU16(buf_ + 7);
}
page_id_t BPlusTreeLeafPage::next_leaf_page_id() const {
    return GetI32(buf_ + 9);
}

void BPlusTreeLeafPage::SetNextLeafPageId(page_id_t next) {
    PutI32(buf_ + 9, next);
}
void BPlusTreeLeafPage::set_num_keys(uint16_t n) {
    PutU16(buf_ + 5, n);
}
void BPlusTreeLeafPage::set_free_space_offset(uint16_t off) {
    PutU16(buf_ + 7, off);
}

uint32_t BPlusTreeLeafPage::FreeSpaceContiguous() const {
    uint32_t dir_end = DirEnd();
    uint32_t data_start = free_space_offset();
    if (dir_end > data_start) {
        return 0; // defensive
    }
    return data_start - dir_end;
}

Slice BPlusTreeLeafPage::KeyAt(uint16_t index) const {
    uint16_t offset = GetU16(buf_ + DirEntryOffset(index));
    uint16_t key_len = GetU16(buf_ + offset);
    return {buf_ + offset + 2, key_len};
}

Slice BPlusTreeLeafPage::ValueAt(uint16_t index) const {
    uint16_t offset = GetU16(buf_ + DirEntryOffset(index));
    uint16_t key_len = GetU16(buf_ + offset);
    uint16_t value_len = GetU16(buf_ + offset + 2 + key_len);
    return {buf_ + offset + 2 + key_len + 2, value_len};
}

BPlusTreeLeafPage::SearchResult BPlusTreeLeafPage::FindKey(const Slice& key,
                                                           const KeyComparator& cmp) const {
    uint16_t lo = 0;
    uint16_t hi = num_keys();
    while (lo < hi) {
        uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        int c = cmp(KeyAt(mid), key);
        if (c == 0) {
            return {mid, true};
        }
        if (c < 0) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return {lo, false}; // lo is the correct insertion index to keep sorted order
}

Status BPlusTreeLeafPage::Insert(const Slice& key, const Slice& value, const KeyComparator& cmp) {
    SearchResult result = FindKey(key, cmp);
    if (result.found) {
        return Status::InvalidArgument("BPlusTreeLeafPage::Insert: key already exists: " +
                                       key.ToString());
    }

    uint32_t record_size =
        2 + static_cast<uint32_t>(key.size()) + 2 + static_cast<uint32_t>(value.size());
    uint32_t needed = record_size + kDirEntrySize;
    if (needed > FreeSpaceContiguous()) {
        Compact(); // reclaim dead space from past removes before giving up
        if (needed > FreeSpaceContiguous()) {
            return Status::ResourceExhausted(
                "BPlusTreeLeafPage::Insert: leaf page full even after compaction");
        }
    }

    uint16_t new_offset = static_cast<uint16_t>(free_space_offset() - record_size);
    PutU16(buf_ + new_offset, static_cast<uint16_t>(key.size()));
    std::memcpy(buf_ + new_offset + 2, key.data(), key.size());
    PutU16(buf_ + new_offset + 2 + key.size(), static_cast<uint16_t>(value.size()));
    std::memcpy(buf_ + new_offset + 2 + key.size() + 2, value.data(), value.size());

    uint16_t n = num_keys();
    for (uint16_t i = n; i > result.index; --i) {
        uint16_t moved = GetU16(buf_ + DirEntryOffset(static_cast<uint16_t>(i - 1)));
        PutU16(buf_ + DirEntryOffset(i), moved);
    }
    PutU16(buf_ + DirEntryOffset(result.index), new_offset);

    set_num_keys(static_cast<uint16_t>(n + 1));
    set_free_space_offset(new_offset);
    return Status::OK();
}

Status BPlusTreeLeafPage::Remove(const Slice& key, const KeyComparator& cmp) {
    SearchResult result = FindKey(key, cmp);
    if (!result.found) {
        return Status::NotFound("BPlusTreeLeafPage::Remove: key not found: " + key.ToString());
    }

    uint16_t n = num_keys();
    for (uint16_t i = result.index; static_cast<uint16_t>(i + 1) < n; ++i) {
        uint16_t moved = GetU16(buf_ + DirEntryOffset(static_cast<uint16_t>(i + 1)));
        PutU16(buf_ + DirEntryOffset(i), moved);
    }
    set_num_keys(static_cast<uint16_t>(n - 1));
    return Status::OK();
}

void BPlusTreeLeafPage::Compact() {
    uint16_t n = num_keys();
    std::vector<std::pair<std::string, std::string>> all;
    all.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        all.emplace_back(KeyAt(i).ToString(), ValueAt(i).ToString());
    }
    page_id_t saved_next = next_leaf_page_id(); // InitNewPage would wipe this
    page_id_t saved_id = page_id();

    InitNewPage(buf_, page_size_, saved_id);
    SetNextLeafPageId(saved_next);
    for (const auto& [k, v] : all) {
        AppendRecordUnchecked(Slice(k), Slice(v));
    }
}

void BPlusTreeLeafPage::AppendRecordUnchecked(const Slice& key, const Slice& value) {
    uint32_t record_size =
        2 + static_cast<uint32_t>(key.size()) + 2 + static_cast<uint32_t>(value.size());
    uint16_t new_offset = static_cast<uint16_t>(free_space_offset() - record_size);
    PutU16(buf_ + new_offset, static_cast<uint16_t>(key.size()));
    std::memcpy(buf_ + new_offset + 2, key.data(), key.size());
    PutU16(buf_ + new_offset + 2 + key.size(), static_cast<uint16_t>(value.size()));
    std::memcpy(buf_ + new_offset + 2 + key.size() + 2, value.data(), value.size());

    uint16_t n = num_keys();
    PutU16(buf_ + DirEntryOffset(n), new_offset);
    set_num_keys(static_cast<uint16_t>(n + 1));
    set_free_space_offset(new_offset);
}

} // namespace engine
#include "engine/log_record.h"

#include <cstring>

#include "engine/byte_utils.h"
#include "engine/crc32c.h"

namespace engine {

void LogRecord::AppendTo(std::string* buf) const {
    size_t start = buf->size();
    size_t total = kFixedHeaderSize + key.size() + 2 + value.size() + kChecksumSize;
    buf->resize(start + total);
    char* p = buf->data() + start;

    PutU64(p + 0, lsn);
    PutU64(p + 8, txn_id);
    p[16] = static_cast<char>(static_cast<uint8_t>(type));
    PutI32(p + 17, page_id);
    PutU16(p + 21, static_cast<uint16_t>(key.size()));
    std::memcpy(p + 23, key.data(), key.size());
    size_t off = 23 + key.size();
    PutU16(p + off, static_cast<uint16_t>(value.size()));
    off += 2;
    std::memcpy(p + off, value.data(), value.size());
    off += value.size();

    uint32_t crc = Crc32c(p, off); // checksum covers everything before the checksum field itself
    PutU32(p + off, crc);
}

StatusOr<LogRecord>
LogRecord::ParseFrom(const char* buf, size_t buf_len, size_t offset, size_t* out_consumed) {
    if (offset + kFixedHeaderSize > buf_len) {
        return Status::Corruption("LogRecord::ParseFrom: buffer too short for a record header");
    }

    const char* p = buf + offset;
    LogRecord record;
    record.lsn = GetU64(p + 0);
    record.txn_id = GetU64(p + 8);
    record.type = static_cast<LogRecordType>(static_cast<uint8_t>(p[16]));
    record.page_id = GetI32(p + 17);
    uint16_t key_len = GetU16(p + 21);
    size_t after_key = offset + 23 + key_len;
    if (after_key + 2 > buf_len) {
        return Status::Corruption("LogRecord::ParseFrom: buffer too short for value_length field");
    }

    uint16_t value_len = GetU16(buf + after_key);
    size_t after_value = after_key + 2 + value_len;
    if (after_value + kChecksumSize > buf_len) {
        return Status::Corruption("LogRecord::ParseFrom: buffer too short for checksum field");
    }

    size_t record_len_before_checksum = after_value - offset;
    uint32_t expected_crc = Crc32c(p, record_len_before_checksum);
    uint32_t stored_crc = GetU32(buf + after_value);
    if (expected_crc != stored_crc) {
        return Status::Corruption(
            "LogRecord::ParseFrom: checksum mismatch -- likely a torn/partial WAL write");
    }

    record.key.assign(p + 23, key_len);
    record.value.assign(buf + after_key + 2, value_len);
    *out_consumed = after_value + kChecksumSize - offset;
    return record;
}

} // namespace engine

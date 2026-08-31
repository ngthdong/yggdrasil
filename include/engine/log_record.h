#pragma once

#include <cstdint>
#include <string>

#include "engine/config.h"
#include "engine/status.h"

namespace engine {

enum class LogRecordType : uint8_t {
    kInvalid = 0,
    kInsert = 1,
    kDelete = 2,
    kBegin = 3,
    kCommit = 4,
    kAbort = 5,
    kCheckpointBegin = 6,
    kCheckpointEnd = 7,
};

// Wire format (little-endian, hand-packed via byte_utils):
//   [0:8)              lsn           (u64)
//   [8:16)             txn_id        (u64)
//   [16:17)            type          (u8)
//   [17:21)            page_id       (i32)
//   [21:23)            key_length    (u16)
//   [23:23+kl)         key bytes
//   [23+kl:25+kl)      value_length  (u16) - 0 for kDelete and the stub types
//   [25+kl:25+kl+vl)   value bytes
//   [.. +4) crc32 over everything from byte 0 up to here
struct LogRecord {
    lsn_t lsn = kInvalidLsn;
    txn_id_t txn_id = kInvalidTxnId;
    LogRecordType type = LogRecordType::kInvalid;
    page_id_t page_id = kInvalidPageId;
    std::string key;
    std::string value;

    static constexpr size_t kFixedHeaderSize = 8 + 8 + 1 + 4 + 2; // up to and including key_length
    static constexpr size_t kChecksumSize = 4;

    // Appends this record's serialized bytes onto the end of *buf.
    void AppendTo(std::string* buf) const;

    // Parses one record starting at buf[offset].
    static StatusOr<LogRecord>
    ParseFrom(const char* buf, size_t buf_len, size_t offset, size_t* out_consumed);
};

} // namespace engine
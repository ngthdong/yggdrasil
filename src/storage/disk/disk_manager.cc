#include "engine/disk_manager.h"
#include "engine/crc32c.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace engine {
namespace {

Status ErrnoStatus(const std::string& what, int err) {
    return Status::IOError(what + ": " + std::strerror(err));
}

Status PreadFullRaw(int fd, void* buf, size_t count, off_t offset) {
    size_t total = 0;
    auto* p = static_cast<char*>(buf);
    while (total < count) {
        ssize_t n = ::pread(fd, p + total, count - total, offset + static_cast<off_t>(total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("pread", errno);
        }
        if (n == 0) {
            return Status::Corruption("short read: file ended before expected boundary");
        }
        total += static_cast<size_t>(n);
    }
    return Status::OK();
}

Status PwriteFullRaw(int fd, const void* buf, size_t count, off_t offset) {
    size_t total = 0;
    const auto* p = static_cast<const char*>(buf);
    while (total < count) {
        ssize_t n = ::pwrite(fd, p + total, count - total, offset + static_cast<off_t>(total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("pwrite", errno);
        }
        total += static_cast<size_t>(n);
    }
    return Status::OK();
}

// Each on-disk slot stores a logical page followed by a 4-byte CRC32C
// checksum trailer. The trailer is transparent to the logical page
// abstraction used by higher-level storage components.
constexpr uint32_t kChecksumTrailerSize = 4;

uint32_t SlotStride(uint32_t logical_page_size) {
    return logical_page_size + kChecksumTrailerSize;
}

Status WriteChecksummedSlot(int fd,
                            page_id_t slot_id,
                            uint32_t logical_page_size,
                            const char* logical_buf) {
    uint32_t stride = SlotStride(logical_page_size);
    std::vector<char> out(stride);
    std::memcpy(out.data(), logical_buf, logical_page_size);
    uint32_t crc = Crc32c(logical_buf, logical_page_size);
    PutU32(out.data() + logical_page_size, crc);
    off_t offset = static_cast<off_t>(slot_id) * static_cast<off_t>(stride);
    return PwriteFullRaw(fd, out.data(), stride, offset);
}

Status
ReadChecksummedSlot(int fd, page_id_t slot_id, uint32_t logical_page_size, char* out_logical_buf) {
    uint32_t stride = SlotStride(logical_page_size);
    std::vector<char> in(stride);
    off_t offset = static_cast<off_t>(slot_id) * static_cast<off_t>(stride);
    Status s = PreadFullRaw(fd, in.data(), stride, offset);
    if (!s.ok()) {
        return s;
    }

    uint32_t stored_crc = GetU32(in.data() + logical_page_size);
    uint32_t computed_crc = Crc32c(in.data(), logical_page_size);
    if (stored_crc != computed_crc) {
        return Status::Corruption(
            "page checksum mismatch at page_id " + std::to_string(slot_id) +
            " and on-disk content does not match its stored CRC32C (bit rot, a partial/torn write, "
            "or other hardware-level corruption)");
    }
    std::memcpy(out_logical_buf, in.data(), logical_page_size);
    return Status::OK();
}

} // namespace

StatusOr<std::unique_ptr<DiskManager>> DiskManager::Open(const std::string& path,
                                                         uint32_t page_size,
                                                         bool create_if_missing,
                                                         SyncPolicy sync_policy) {
    int flags = O_RDWR;
    if (create_if_missing) {
        flags |= O_CREAT;
    }

    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        if (errno == ENOENT) {
            return Status::NotFound("database file does not exist: " + path);
        }
        return ErrnoStatus("open(" + path + ")", errno);
    }

    off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        Status s = ErrnoStatus("lseek", errno);
        ::close(fd);
        return s;
    }

    if (file_size == 0) {
        // Fresh file: create and durably write a brand-new superblock before
        // returning, so a crash immediately after Open() still leaves a valid file.
        Superblock sb;
        sb.page_size = page_size;
        sb.page_count = 1;

        std::vector<char> buf(page_size, 0);
        sb.SerializeTo(buf.data(), page_size);

        Status s = WriteChecksummedSlot(fd, kSuperblockPageId, page_size, buf.data());
        if (!s.ok()) {
            ::close(fd);
            return s;
        }
        if (::fsync(fd) != 0) {
            Status s2 = ErrnoStatus("fsync(superblock)", errno);
            ::close(fd);
            return s2;
        }

        return std::unique_ptr<DiskManager>(new DiskManager(fd, sb, sync_policy));
    }

    // Peek at the on-disk page_size via a small raw (pre-checksum) read of
    // the superblock header, before computing a checksummed-slot stride from
    // the caller-supplied page_size. Without this, a genuine page_size
    // mismatch would make ReadChecksummedSlot below read the wrong stride --
    // misreading the file and surfacing as a spurious Corruption instead of
    // the InvalidArgument this is supposed to be.
    constexpr size_t kSuperblockProbeSize = 32; // magic(4)+version(4)+page_size(4)+page_count(4)+free_list(4)+root(4)+lsn(8)
    std::vector<char> probe_buf(kSuperblockProbeSize, 0);
    Status probe_s = PreadFullRaw(fd, probe_buf.data(), probe_buf.size(), 0);
    if (!probe_s.ok()) {
        ::close(fd);
        return probe_s;
    }
    StatusOr<Superblock> probe_sb_or =
        Superblock::DeserializeFrom(probe_buf.data(), static_cast<uint32_t>(probe_buf.size()));
    if (!probe_sb_or.ok()) {
        ::close(fd);
        return probe_sb_or.status();
    }
    if (probe_sb_or.value().page_size != page_size) {
        ::close(fd);
        return Status::InvalidArgument("page_size mismatch: file was created with page_size=" +
                                       std::to_string(probe_sb_or.value().page_size) +
                                       " but Open() was called with " + std::to_string(page_size));
    }

    std::vector<char> logical_buf(page_size, 0);
    Status read_s = ReadChecksummedSlot(fd, kSuperblockPageId, page_size, logical_buf.data());
    if (!read_s.ok()) {
        ::close(fd);
        return read_s;
    }

    StatusOr<Superblock> sb_or = Superblock::DeserializeFrom(logical_buf.data(), page_size);
    if (!sb_or.ok()) {
        ::close(fd);
        return sb_or.status();
    }

    Superblock sb = sb_or.value();
    uint64_t expected_size = static_cast<uint64_t>(SlotStride(sb.page_size)) * sb.page_count;
    if (static_cast<uint64_t>(file_size) != expected_size) {
        ::close(fd);
        return Status::Corruption(
            "file size " + std::to_string(file_size) +
            " does not match superblock's page_count * (page_size + checksum trailer) (" +
            std::to_string(expected_size) + "); file may be truncated or torn");
    }

    return std::unique_ptr<DiskManager>(new DiskManager(fd, sb, sync_policy));
}

DiskManager::DiskManager(int fd, Superblock superblock, SyncPolicy sync_policy)
    : fd_(fd), superblock_(superblock), sync_policy_(sync_policy) {}

DiskManager::~DiskManager() {
    if (!shutdown_) {
        Shutdown();
    }
}

Status DiskManager::ReadPage(page_id_t page_id, char* out_buf) const {
    if (page_id < 0 || static_cast<uint32_t>(page_id) >= superblock_.page_count) {
        return Status::InvalidArgument(
            "ReadPage: page_id " + std::to_string(page_id) +
            " out of range (page_count=" + std::to_string(superblock_.page_count) + ")");
    }

    return ReadChecksummedSlot(fd_, page_id, superblock_.page_size, out_buf);
}

Status DiskManager::WritePage(page_id_t page_id, const char* in_buf) {
    if (page_id == kSuperblockPageId) {
        return Status::InvalidArgument(
            "WritePage: page 0 is the superblock and is managed internally");
    }
    if (page_id < 0 || static_cast<uint32_t>(page_id) >= superblock_.page_count) {
        return Status::InvalidArgument(
            "WritePage: page_id " + std::to_string(page_id) +
            " out of range (page_count=" + std::to_string(superblock_.page_count) + ")");
    }

    Status s = WriteChecksummedSlot(fd_, page_id, superblock_.page_size, in_buf);
    if (!s.ok()) {
        return s;
    }

    return SyncIfNeeded();
}

StatusOr<page_id_t> DiskManager::AllocatePage() {
    page_id_t new_id = static_cast<page_id_t>(superblock_.page_count);
    std::vector<char> zero(superblock_.page_size, 0);
    Status s = WriteChecksummedSlot(fd_, new_id, superblock_.page_size, zero.data());
    if (!s.ok()) {
        return s;
    }

    superblock_.page_count += 1;
    s = WriteSuperblock();
    if (!s.ok()) {
        return s;
    }

    return new_id;
}

Status DiskManager::WriteSuperblock() {
    std::vector<char> buf(superblock_.page_size, 0);
    superblock_.SerializeTo(buf.data(), superblock_.page_size);
    Status s = WriteChecksummedSlot(fd_, kSuperblockPageId, superblock_.page_size, buf.data());
    if (!s.ok()) {
        return s;
    }

    return SyncIfNeeded();
}

Status DiskManager::SyncIfNeeded() {
    if (sync_policy_ == SyncPolicy::kEveryWrite) {
        if (::fsync(fd_) != 0) {
            return ErrnoStatus("fsync", errno);
        }
    }

    return Status::OK();
}

Status DiskManager::Shutdown() {
    if (shutdown_) {
        return Status::OK();
    }
    Status s = WriteSuperblock();
    if (::fsync(fd_) != 0 && s.ok()) {
        s = ErrnoStatus("fsync(shutdown)", errno);
    }

    ::close(fd_);
    shutdown_ = true;
    return s;
}

} // namespace engine
#include "engine/disk_manager.h"

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

} // namespace

StatusOr<std::unique_ptr<DiskManager>> DiskManager::Open(const std::string& path,
                                                         uint32_t page_size, bool create_if_missing,
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

        ssize_t written = ::pwrite(fd, buf.data(), page_size, 0);
        if (written < 0 || static_cast<uint32_t>(written) != page_size) {
            Status s = ErrnoStatus("pwrite(superblock)", errno);
            ::close(fd);
            return s;
        }
        if (::fsync(fd) != 0) {
            Status s = ErrnoStatus("fsync(superblock)", errno);
            ::close(fd);
            return s;
        }
        return std::unique_ptr<DiskManager>(new DiskManager(fd, sb, sync_policy));
    }

    // Existing file: we don't know the on-disk page_size yet
    // so read the superblock page and deserialize it to find out.
    size_t read_len = std::min<uint64_t>(page_size, static_cast<uint64_t>(file_size));
    std::vector<char> buf(read_len, 0);
    ssize_t nread = ::pread(fd, buf.data(), read_len, 0);
    if (nread < 0) {
        Status s = ErrnoStatus("pread(superblock)", errno);
        ::close(fd);
        return s;
    }
    if (static_cast<size_t>(nread) != read_len) {
        ::close(fd);
        return Status::Corruption("short read on superblock page of " + path);
    }

    StatusOr<Superblock> sb_or =
        Superblock::DeserializeFrom(buf.data(), static_cast<uint32_t>(read_len));
    if (!sb_or.ok()) {
        ::close(fd);
        return sb_or.status();
    }
    Superblock sb = sb_or.value();

    if (sb.page_size != page_size) {
        ::close(fd);
        return Status::InvalidArgument(
            "page_size mismatch: file was created with page_size=" + std::to_string(sb.page_size) +
            " but Open() was called with " + std::to_string(page_size));
    }

    uint64_t expected_size = static_cast<uint64_t>(sb.page_size) * sb.page_count;
    if (static_cast<uint64_t>(file_size) != expected_size) {
        ::close(fd);
        return Status::Corruption("file size " + std::to_string(file_size) +
                                  " does not match superblock's page_count * page_size (" +
                                  std::to_string(expected_size) +
                                  ") -- file may be truncated or torn");
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

Status DiskManager::PreadFull(void* buf, size_t count, off_t offset) const {
    size_t total = 0;
    auto* p = static_cast<char*>(buf);
    while (total < count) {
        ssize_t n = ::pread(fd_, p + total, count - total, offset + static_cast<off_t>(total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("pread", errno);
        }
        if (n == 0) {
            // Hit EOF before reading `count` bytes
            return Status::Corruption("short read: file ended before page boundary");
        }
        total += static_cast<size_t>(n);
    }
    return Status::OK();
}

Status DiskManager::PwriteFull(const void* buf, size_t count, off_t offset) const {
    size_t total = 0;
    const auto* p = static_cast<const char*>(buf);
    while (total < count) {
        ssize_t n = ::pwrite(fd_, p + total, count - total, offset + static_cast<off_t>(total));
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

Status DiskManager::ReadPage(page_id_t page_id, char* out_buf) {
    if (page_id < 0 || static_cast<uint32_t>(page_id) >= superblock_.page_count) {
        return Status::InvalidArgument(
            "ReadPage: page_id " + std::to_string(page_id) +
            " out of range (page_count=" + std::to_string(superblock_.page_count) + ")");
    }
    off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(superblock_.page_size);
    return PreadFull(out_buf, superblock_.page_size, offset);
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
    off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(superblock_.page_size);
    Status s = PwriteFull(in_buf, superblock_.page_size, offset);
    if (!s.ok()) {
        return s;
    }
    return SyncIfNeeded();
}

StatusOr<page_id_t> DiskManager::AllocatePage() {
    page_id_t new_id = static_cast<page_id_t>(superblock_.page_count);
    off_t offset = static_cast<off_t>(new_id) * static_cast<off_t>(superblock_.page_size);

    std::vector<char> zero(superblock_.page_size, 0);
    Status s = PwriteFull(zero.data(), superblock_.page_size, offset);
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
    Status s = PwriteFull(buf.data(), superblock_.page_size, 0);
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
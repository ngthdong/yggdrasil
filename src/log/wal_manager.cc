#include "engine/wal_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace engine {

namespace {
Status ErrnoStatus(const std::string& what, int err) {
    return Status::IOError(what + ": " + std::strerror(err));
}
} // namespace

StatusOr<std::unique_ptr<WalManager>> WalManager::Open(const std::string& path,
                                                       SyncPolicy sync_policy) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return ErrnoStatus("open(" + path + ")", errno);
    }
    return std::unique_ptr<WalManager>(new WalManager(fd, sync_policy));
}

WalManager::WalManager(int fd, SyncPolicy sync_policy) : fd_(fd), sync_policy_(sync_policy) {}

WalManager::~WalManager() {
    if (!shutdown_) {
        Shutdown();
    }
}

StatusOr<lsn_t> WalManager::AppendLogRecord(LogRecordType type,
                                            page_id_t page_id,
                                            const Slice& key,
                                            const Slice& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    LogRecord record;
    record.lsn = next_lsn_++;
    record.type = type;
    record.page_id = page_id;
    record.key = key.ToString();
    record.value = value.ToString();

    record.AppendTo(&buffer_);
    highest_appended_lsn_ = record.lsn;
    highest_appended_offset_ = buffer_.size();
    return record.lsn;
}

Status WalManager::Flush(lsn_t lsn) {
    std::unique_lock<std::mutex> lock(mutex_);

    while (lsn > durable_lsn_) {
        if (flush_in_progress_) {
            cv_.wait(lock);
            continue;
        }

        flush_in_progress_ = true;
        size_t write_from = durable_offset_;
        size_t write_upto = highest_appended_offset_;
        lsn_t write_upto_lsn = highest_appended_lsn_;

        // Copy the pending WAL bytes before releasing the lock, since concurrent
        // appends may reallocate buffer_ and invalidate pointers. This avoids
        // use-after-free during unlocked I/O at the cost of one copy per flush.
        std::string to_write = buffer_.substr(write_from, write_upto - write_from);
        lock.unlock();

        Status s = PwriteAll(to_write.data(), to_write.size(), static_cast<off_t>(write_from));
        if (s.ok() && sync_policy_ == SyncPolicy::kEveryFlush) {
            if (::fsync(fd_) != 0) {
                s = ErrnoStatus("fsync", errno);
            }
        }

        lock.lock();
        flush_in_progress_ = false;
        if (s.ok()) {
            durable_offset_ = write_upto;
            durable_lsn_ = write_upto_lsn;
        }
        cv_.notify_all();

        if (!s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

lsn_t WalManager::durable_lsn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return durable_lsn_;
}

Status WalManager::PwriteAll(const char* data, size_t len, off_t offset) const {
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::pwrite(fd_, data + total, len - total, offset + static_cast<off_t>(total));
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

Status WalManager::RecycleAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (highest_appended_offset_ != durable_offset_) {
        return Status::InvalidArgument(
            "WalManager::RecycleAll pending (non-durable) data exists; flush before recycling");
    }
    if (::ftruncate(fd_, 0) != 0) {
        return ErrnoStatus("ftruncate", errno);
    }
    buffer_.clear();
    highest_appended_offset_ = 0;
    durable_offset_ = 0;

    return Status::OK();
}

Status WalManager::Shutdown() {
    if (shutdown_) {
        return Status::OK();
    }
    lsn_t target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = highest_appended_lsn_;
    }
    Status flush_s = Flush(target);
    ::close(fd_);
    shutdown_ = true;
    return flush_s;
}

} // namespace engine
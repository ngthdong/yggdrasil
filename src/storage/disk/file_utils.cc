#include "engine/file_utils.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace engine {
namespace {

Status ErrnoStatus(const std::string& what, int err) {
    return Status::IOError(what + ": " + std::strerror(err));
}

} // namespace

Status CopyFile(const std::string& src, const std::string& dst) {
    int src_fd = ::open(src.c_str(), O_RDONLY);
    if (src_fd < 0) {
        return ErrnoStatus("open(" + src + ")", errno);
    }

    int dst_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        Status s = ErrnoStatus("open(" + dst + ")", errno);
        ::close(src_fd);
        return s;
    }

    constexpr size_t kChunkSize = 1u << 20; // 1 MiB
    std::array<char, kChunkSize> buf{};
    Status status = Status::OK();
    for (;;) {
        ssize_t nread = ::read(src_fd, buf.data(), buf.size());
        if (nread < 0) {
            status = ErrnoStatus("read(" + src + ")", errno);
            break;
        }
        if (nread == 0) {
            break; // EOF
        }

        size_t written = 0;
        while (written < static_cast<size_t>(nread)) {
            ssize_t n = ::write(dst_fd, buf.data() + written, static_cast<size_t>(nread) - written);
            if (n < 0) {
                status = ErrnoStatus("write(" + dst + ")", errno);
                break;
            }
            written += static_cast<size_t>(n);
        }
        if (!status.ok()) {
            break;
        }
    }

    ::close(src_fd);
    ::close(dst_fd);
    if (!status.ok()) {
        std::remove(dst.c_str());
    }
    return status;
}

} // namespace engine

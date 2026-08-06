#pragma once

#include <string>
#include <utility>

namespace engine {

// Status is returned by every public and internal API in the
// engine instead of throwing exceptions.
class Status {
  public:
    enum class Code {
        kOk = 0,
        kNotFound,
        kCorruption,
        kIOError,
        kInvalidArgument,
        kResourceExhausted,
        kAborted,
        kNotSupported,
    };

    Status() : code_(Code::kOk) {}

    static Status OK() {
        return Status();
    }

    static Status NotFound(std::string msg) {
        return Status(Code::kNotFound, std::move(msg));
    }
    static Status Corruption(std::string msg) {
        return Status(Code::kCorruption, std::move(msg));
    }
    static Status IOError(std::string msg) {
        return Status(Code::kIOError, std::move(msg));
    }
    static Status InvalidArgument(std::string msg) {
        return Status(Code::kInvalidArgument, std::move(msg));
    }
    static Status ResourceExhausted(std::string msg) {
        return Status(Code::kResourceExhausted, std::move(msg));
    }
    static Status Aborted(std::string msg) {
        return Status(Code::kAborted, std::move(msg));
    }
    static Status NotSupported(std::string msg) {
        return Status(Code::kNotSupported, std::move(msg));
    }

    bool ok() const {
        return code_ == Code::kOk;
    }
    Code code() const {
        return code_;
    }
    const std::string& message() const {
        return message_;
    }

    std::string ToString() const {
        if (ok())
            return "OK";
        return std::string(CodeName(code_)) + ": " + message_;
    }

  private:
    Status(Code code, std::string msg) : code_(code), message_(std::move(msg)) {}

    static const char* CodeName(Code c) {
        switch (c) {
        case Code::kOk:
            return "OK";
        case Code::kNotFound:
            return "NotFound";
        case Code::kCorruption:
            return "Corruption";
        case Code::kIOError:
            return "IOError";
        case Code::kInvalidArgument:
            return "InvalidArgument";
        case Code::kResourceExhausted:
            return "ResourceExhausted";
        case Code::kAborted:
            return "Aborted";
        case Code::kNotSupported:
            return "NotSupported";
        }
        return "Unknown";
    }

    Code code_;
    std::string message_;
};

template <typename T> class StatusOr {
  public:
    StatusOr(Status status) : status_(std::move(status)) {}
    StatusOr(T value) : status_(Status::OK()), value_(std::move(value)) {}

    bool ok() const {
        return status_.ok();
    }
    const Status& status() const {
        return status_;
    }

    const T& value() const& {
        return value_;
    }
    T& value() & {
        return value_;
    }
    T&& value() && {
        return std::move(value_);
    }

  private:
    Status status_;
    T value_{};
};

} // namespace engine

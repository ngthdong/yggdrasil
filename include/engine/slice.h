#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace engine {

// Slice is a non-owning view over a byte range, used everywhere a key or
// value is passed around so the engine never has to guess who owns the
// backing memory.

// IMPORTANT: a Slice's validity is tied to whatever buffer it points into.
// Once that buffer is freed, mutated, or (later, once pages exist) unpinned,
// the Slice is dangling. Callers must copy into a std::string if they need
// the bytes to outlive the source buffer.
class Slice {
  public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* data, size_t size) : data_(data), size_(size) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Slice(const char* s) : data_(s), size_(strlen(s)) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    std::string ToString() const { return std::string(data_, size_); }
    std::string_view ToStringView() const { return std::string_view(data_, size_); }

    bool operator==(const Slice& other) const {
        return size_ == other.size_ && memcmp(data_, other.data_, size_) == 0;
    }
    bool operator!=(const Slice& other) const { return !(*this == other); }

    int Compare(const Slice& other) const {
        const size_t min_len = size_ < other.size_ ? size_ : other.size_;
        int r = min_len == 0 ? 0 : memcmp(data_, other.data_, min_len);
        if (r == 0) {
            if (size_ < other.size_)
                r = -1;
            else if (size_ > other.size_)
                r = 1;
        }
        return r;
    }

  private:
    const char* data_;
    size_t size_;
};

} // namespace engine

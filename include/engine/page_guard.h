#pragma once

#include "engine/buffer_pool_manager.h"
#include "engine/config.h"

namespace engine {

// PageGuard is the RAII wrapper around a pinned page: construct via
// FetchPageGuarded/NewPageGuarded below, and it unpins automatically on
// destruction.
//
// IMPORTANT: mutating mutable_data() does NOT automatically mark the page
// dirty.
class PageGuard {
  public:
    PageGuard() = default;

    PageGuard(BufferPoolManager* bpm, page_id_t page_id, char* data)
        : bpm_(bpm), page_id_(page_id), data_(data) {}

    ~PageGuard() {
        Reset();
    }

    PageGuard(PageGuard&& other) noexcept {
        MoveFrom(other);
    }
    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            MoveFrom(other);
        }
        return *this;
    }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    bool is_valid() const {
        return bpm_ != nullptr;
    }
    page_id_t page_id() const {
        return page_id_;
    }
    const char* data() const {
        return data_;
    }
    char* mutable_data() {
        return data_;
    }
    void MarkDirty() {
        dirty_ = true;
    }
    bool is_dirty() const {
        return dirty_;
    }

    void Reset() {
        if (bpm_ != nullptr) {
            bpm_->UnpinPage(page_id_, dirty_);
            bpm_ = nullptr;
            data_ = nullptr;
            dirty_ = false;
        }
    }

  private:
    void MoveFrom(PageGuard& other) {
        bpm_ = other.bpm_;
        page_id_ = other.page_id_;
        data_ = other.data_;
        dirty_ = other.dirty_;
        other.bpm_ = nullptr;
        other.data_ = nullptr;
        other.dirty_ = false;
    }

    BufferPoolManager* bpm_ = nullptr;
    page_id_t page_id_ = kInvalidPageId;
    char* data_ = nullptr;
    bool dirty_ = false;
};

// Free functions (not BufferPoolManager methods) purely to avoid a
// circular header dependency between buffer_pool_manager.h and
// page_guard.h. No privileged access, built entirely on the public API.
inline StatusOr<PageGuard> FetchPageGuarded(BufferPoolManager* bpm, page_id_t page_id) {
    StatusOr<char*> data_or = bpm->FetchPage(page_id);
    if (!data_or.ok())
        return data_or.status();
    return PageGuard(bpm, page_id, data_or.value());
}

inline StatusOr<PageGuard> NewPageGuarded(BufferPoolManager* bpm, page_id_t* out_page_id) {
    StatusOr<char*> data_or = bpm->NewPage(out_page_id);
    if (!data_or.ok())
        return data_or.status();
    return PageGuard(bpm, *out_page_id, data_or.value());
}

} // namespace engine
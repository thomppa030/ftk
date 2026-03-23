// fjell_shared_ref.hpp — SharedRef<T> for shared ownership of user-created objects
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include <cstdint>
#include <utility>

extern "C" void fjell_shared_ref_acquire(uint32_t ref_token);
extern "C" void fjell_shared_ref_release(uint32_t ref_token);
extern "C" void* fjell_shared_ref_resolve(uint32_t ref_token);

namespace fjell {

struct SharedRefData {
    uint32_t ref_token{0};
};

template <typename T>
class SharedRef {
public:
    SharedRef() = default;

    ~SharedRef() {
        if (data_.ref_token != 0) fjell_shared_ref_release(data_.ref_token);
    }

    SharedRef(const SharedRef& other) : data_(other.data_) {
        if (data_.ref_token != 0) fjell_shared_ref_acquire(data_.ref_token);
    }

    SharedRef& operator=(const SharedRef& other) {
        if (this != &other) {
            if (data_.ref_token != 0) fjell_shared_ref_release(data_.ref_token);
            data_ = other.data_;
            if (data_.ref_token != 0) fjell_shared_ref_acquire(data_.ref_token);
        }
        return *this;
    }

    SharedRef(SharedRef&& other) noexcept : data_(other.data_) {
        other.data_ = {};
    }

    SharedRef& operator=(SharedRef&& other) noexcept {
        if (this != &other) {
            if (data_.ref_token != 0) fjell_shared_ref_release(data_.ref_token);
            data_ = other.data_;
            other.data_ = {};
        }
        return *this;
    }

    explicit operator bool() const { return get() != nullptr; }

    [[nodiscard]] T* get() const {
        if (data_.ref_token == 0) return nullptr;
        return static_cast<T*>(fjell_shared_ref_resolve(data_.ref_token));
    }

    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }

    [[nodiscard]] uint32_t ref_token() const { return data_.ref_token; }

    SharedRefData data_;
};

} // namespace fjell

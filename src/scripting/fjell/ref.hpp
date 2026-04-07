// fjell_ref.hpp — Ref<T> for owning references to engine resources
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/api.h"

#include <cstdint>
#include <utility>

extern "C" FJELL_SDK_EXPORT void fjell_ref_acquire(uint32_t ref_token);
extern "C" FJELL_SDK_EXPORT void fjell_ref_release(uint32_t ref_token);
extern "C" FJELL_SDK_EXPORT void* fjell_ref_resolve(uint32_t handle_id);

namespace fjell {

struct RefData {
    uint32_t handle_id{0};
    uint32_t ref_token{0};
};

template <typename T>
class Ref {
public:
    Ref() = default;

    ~Ref() {
        if (data_.ref_token != 0) fjell_ref_release(data_.ref_token);
    }

    Ref(const Ref& other) : data_(other.data_) {
        if (data_.ref_token != 0) fjell_ref_acquire(data_.ref_token);
    }

    Ref& operator=(const Ref& other) {
        if (this != &other) {
            if (data_.ref_token != 0) fjell_ref_release(data_.ref_token);
            data_ = other.data_;
            if (data_.ref_token != 0) fjell_ref_acquire(data_.ref_token);
        }
        return *this;
    }

    Ref(Ref&& other) noexcept : data_(other.data_) {
        other.data_ = {};
    }

    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            if (data_.ref_token != 0) fjell_ref_release(data_.ref_token);
            data_ = other.data_;
            other.data_ = {};
        }
        return *this;
    }

    explicit operator bool() const { return get() != nullptr; }

    [[nodiscard]] T* get() const {
        if (data_.handle_id == 0) return nullptr;
        return static_cast<T*>(fjell_ref_resolve(data_.handle_id));
    }

    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }

    [[nodiscard]] uint32_t handle_id() const { return data_.handle_id; }
    [[nodiscard]] uint32_t ref_token() const { return data_.ref_token; }

    RefData data_;
};

} // namespace fjell

// fjell_weak_ref.hpp — WeakRef<T> for safe, non-owning references
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" FJELL_SDK_EXPORT void* fjell_resolve_weak_ref(uint32_t id, uint32_t generation);

namespace fjell {

struct WeakRefData {
    uint32_t id{0};
    uint32_t generation{0};
};

template <typename T>
class WeakRef {
public:
    WeakRef() = default;

    explicit operator bool() const { return get() != nullptr; }

    [[nodiscard]] T* get() const {
        if (data_.id == 0) return nullptr;
        return static_cast<T*>(fjell_resolve_weak_ref(data_.id, data_.generation));
    }

    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }

    [[nodiscard]] uint32_t id() const { return data_.id; }
    [[nodiscard]] uint32_t generation() const { return data_.generation; }

    bool operator==(const WeakRef& other) const = default;

    WeakRefData data_;
};

} // namespace fjell

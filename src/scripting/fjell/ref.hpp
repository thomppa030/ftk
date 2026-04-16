// fjell_ref.hpp — Ref<T> for safe access to engine-owned assets
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" FJELL_SDK_EXPORT void* fjell_ref_resolve(uint32_t handle_id, uint8_t asset_type_id);
extern "C" FJELL_SDK_EXPORT void  fjell_log_error(const char* msg);

namespace fjell {

struct RefData {
    uint32_t handle_id{0};
    uint8_t  asset_type_id{0};   // set by engine at deserialization/inspector time
    uint8_t  reserved_[3]{};     // future use, keeps sizeof(RefData) == 8
};

template <typename T>
class Ref {
public:
    Ref() = default;

    explicit operator bool() const { return data_.handle_id != 0; }

    [[nodiscard]] T* get() const {
        if (data_.handle_id == 0) return nullptr;
        return static_cast<T*>(fjell_ref_resolve(data_.handle_id, data_.asset_type_id));
    }

    T* operator->() const {
        T* ptr = get();
        if (!ptr) {
            fjell_log_error("Ref::operator-> on null ref — assign it in the inspector");
        }
        return ptr;
    }

    T& operator*() const {
        T* ptr = get();
        if (!ptr) {
            fjell_log_error("Ref::operator* on null ref — assign it in the inspector");
        }
        return *ptr;
    }

    [[nodiscard]] uint32_t handle_id() const { return data_.handle_id; }

    RefData data_;
};

} // namespace fjell

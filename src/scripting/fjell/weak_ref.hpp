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

/// Non-owning reference to another object that detects destruction. The
/// generation counter means a handle to a destroyed object resolves to null
/// rather than to whatever later reused its slot — so a stale `WeakRef` is
/// safe, unlike a raw pointer.
///
/// Use it for "the enemy I am chasing" style links that must not keep the
/// target alive and must survive the target dying.
///
/// @code
/// [[fj::property]] fjell::WeakRef<Enemy> target;
///
/// void tick(float dt) override {
///     if (Enemy* e = target.get()) { chase(*e); }
///     else { idle(); }   // target died or was never assigned
/// }
/// @endcode
template <typename T>
class WeakRef {
public:
    WeakRef() = default;

    /// Whether the target is still alive. Equivalent to `get() != nullptr`,
    /// and involves the same resolve — cache `get()` if you need the pointer too.
    explicit operator bool() const { return get() != nullptr; }

    /// Resolve to the target.
    /// @return Pointer to the object, or null if it was destroyed or the ref
    ///         was never assigned. Re-check after anything that could destroy
    ///         it; do not cache the result across frames.
    [[nodiscard]] T* get() const {
        if (data_.id == 0) return nullptr;
        return static_cast<T*>(fjell_resolve_weak_ref(data_.id, data_.generation));
    }

    /// Access the target's members. Does NOT null-check — dereferences null if
    /// the target is gone. Use `get()` with a check unless liveness is certain.
    T* operator->() const { return get(); }

    /// Dereference the target. Does NOT null-check; see `operator->`.
    T& operator*() const { return *get(); }

    /// Raw object ID. 0 means unassigned. Non-zero does not imply alive.
    [[nodiscard]] uint32_t id() const { return data_.id; }

    /// Generation stamp captured when the ref was assigned. Resolution fails
    /// once the engine's generation for this ID moves past it.
    [[nodiscard]] uint32_t generation() const { return data_.generation; }

    bool operator==(const WeakRef& other) const = default;

    WeakRefData data_;
};

} // namespace fjell

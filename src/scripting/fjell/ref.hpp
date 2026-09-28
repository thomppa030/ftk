// ref.hpp — Ref<T>: a reference to a component on a node, which knows when it is gone
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/api.h"
#include "fjell/component_id.hpp"

#include <atomic>
#include <cstdint>
#include <exception>

extern "C" {
FJELL_SDK_EXPORT void* fjell_ref_resolve(uint32_t node_id, uint32_t generation, uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_scene_generation(uint32_t node_id);
FJELL_SDK_EXPORT const std::atomic<uint64_t>* fjell_removal_epoch();
FJELL_SDK_EXPORT void fjell_ref_gone(const void* ref, uint32_t node_id, uint32_t type_id);
}

namespace fjell {

/// What a Ref holds. The engine reads and writes it in place: it saves the
/// node, and sets the node and its generation when the field is loaded or
/// picked in the inspector.
struct RefData {
    uint32_t node_id{0};
    /// Which life of that node: another node that later takes the id is not
    /// the one meant.
    uint32_t generation{0};
    /// What it last found, while `epoch` is the engine's removal epoch. Never
    /// a null: a miss is looked up again next time.
    void* cached{nullptr};
    uint64_t epoch{0};
};

/// Thrown by `->` and `*` on a Ref whose target is gone. The engine has
/// already been told by then, and stops the game with the script, the field,
/// the node and the hook named; the throw only ends what the script was doing.
/// Catching it does not keep the game running.
struct RefGone : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "a Ref was used after its target was gone";
    }
};

namespace detail {

/// Tells the engine a Ref at `ref` was used with nothing there, then throws.
/// The throw is here, in the game library, rather than in the engine's C
/// export, which a compiler may assume never throws.
[[noreturn]] inline void ref_gone(const void* ref, uint32_t node_id, uint32_t type_id) {
    fjell_ref_gone(ref, node_id, type_id);
    throw RefGone{};
}

/// Moves whenever anything a Ref could point at goes away; until it does, a
/// Ref keeps what it found.
[[nodiscard]] inline uint64_t removal_epoch() {
    static const std::atomic<uint64_t>* const counter = fjell_removal_epoch();
    return counter->load(std::memory_order_relaxed);
}

} // namespace detail

/// A reference to the component of type T on a node: an engine component
/// (`RigidbodyComponent`, `MeshComponent`, ...) or a script component. Hold it
/// as a `[[fj::property]]` and pick the node in the inspector, which offers
/// the nodes that carry a T; or make one from a node id.
///
/// It knows when its target is gone: the node destroyed or taken out of the
/// scene, the component removed, the game library reloading. It never finds
/// another node that later took the same id. A node put back by undo is the
/// same node, and is found again.
///
/// Looking it up is cheap: it keeps what it found until anything anywhere
/// goes away, and only then asks the engine again.
///
/// @code
/// [[fj::property]] fjell::Ref<PlayerStats> player;   // always there
/// [[fj::property]] fjell::Ref<Enemy> target;          // may be killed
///
/// void tick(float) override {
///     player->stamina += 1.0f;
///     if (auto* enemy = target.get()) chase(*enemy);
/// }
/// @endcode
template <typename T>
class Ref {
public:
    Ref() = default;

    /// The T on node `node_id` as it is now: a later node that takes the id
    /// is not this one.
    explicit Ref(uint32_t node_id) : data_{node_id, fjell_scene_generation(node_id), nullptr, 0} {}

    /// The target, or null when there is none: never set, gone, or the node no
    /// longer carries a T.
    [[nodiscard]] T* get() const {
        if (data_.node_id == 0) return nullptr;
        const uint64_t now = detail::removal_epoch();
        if (data_.cached != nullptr && data_.epoch == now) return static_cast<T*>(data_.cached);
        data_.cached = fjell_ref_resolve(data_.node_id, data_.generation,
                                         detail::component_type_id<T>());
        data_.epoch = now;
        return static_cast<T*>(data_.cached);
    }

    /// The target's members. Using a Ref whose target is gone stops the game,
    /// shipped builds included, naming the script, the field and the node: a
    /// target that can be missing is checked with `if (ref)` or `get()` first.
    T* operator->() const {
        T* target = get();
        if (target == nullptr) detail::ref_gone(this, data_.node_id, detail::component_type_id<T>());
        return target;
    }

    /// The target itself, under the same rule as `->`.
    T& operator*() const { return *operator->(); }

    /// Whether the target is there now.
    [[nodiscard]] explicit operator bool() const { return get() != nullptr; }

    /// The node it refers to; 0 when never set. Not a check that it is there.
    [[nodiscard]] uint32_t node_id() const { return data_.node_id; }

    /// The same node in the same life.
    [[nodiscard]] bool operator==(const Ref& other) const {
        return data_.node_id == other.data_.node_id && data_.generation == other.data_.generation;
    }

private:
    mutable RefData data_;
};

} // namespace fjell

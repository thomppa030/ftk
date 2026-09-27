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

/// Reference to an engine-owned asset — a preset, audio clip, VFX effect,
/// input action, surface, and so on. Declare it as a `[[fj::property]]` and the
/// inspector shows an asset picker for the right type; the chosen asset is
/// saved with the scene.
///
/// A `Ref<Material>` takes any material unless it says which shader types it
/// can use: `[[fj::shader_type(water)]]` (or `(water, fog)`) lists only those
/// materials in the picker and refuses others dropped on the field.
///
/// A `Ref` does not own or load anything: it holds a handle and resolves
/// through the engine on each access, so it stays correct across asset
/// hot-reload. Always check `valid()` before use — an unassigned field is the
/// normal state, not an error.
///
/// @code
/// class Spawner : public fjell::ScriptComponent {
///     FJELL_COMPONENT(Spawner)
/// public:
///     [[fj::property]] fjell::Ref<fjell::PresetAsset> enemy;
///
///     void begin_play() override {
///         if (enemy.valid()) fjell::scene::spawn(enemy, {0, 1, 0});
///     }
/// };
/// @endcode
template <typename T>
class Ref {
public:
    Ref() = default;

    /// Whether an asset has been assigned. False for a field left empty in
    /// the inspector.
    [[nodiscard]] bool valid() const { return data_.handle_id != 0; }

    /// Same as `valid()`, for `if (my_ref)`.
    explicit operator bool() const { return valid(); }

    /// Resolve to the underlying asset.
    /// @return Pointer to the asset, or null if unassigned or not yet loaded.
    ///         The safe accessor — unlike `operator->`, it does not log.
    [[nodiscard]] T* get() const {
        if (data_.handle_id == 0) return nullptr;
        return static_cast<T*>(fjell_ref_resolve(data_.handle_id, data_.asset_type_id));
    }

    /// Access the asset's members. Logs an error and returns null when the ref
    /// is unassigned, so a forgotten inspector assignment is reported rather
    /// than silently crashing. Prefer `get()` with a null check where an
    /// unassigned ref is a legitimate state.
    T* operator->() const {
        T* ptr = get();
        if (!ptr) {
            fjell_log_error("Ref::operator-> on null ref — assign it in the inspector");
        }
        return ptr;
    }

    /// Dereference the asset. Logs an error when unassigned; the returned
    /// reference is invalid in that case, so guard with `valid()` first.
    T& operator*() const {
        T* ptr = get();
        if (!ptr) {
            fjell_log_error("Ref::operator* on null ref — assign it in the inspector");
        }
        return *ptr;
    }

    /// Raw asset handle, for the engine APIs that take one directly.
    /// 0 means unassigned.
    [[nodiscard]] uint32_t handle_id() const { return data_.handle_id; }

    RefData data_;
};

} // namespace fjell

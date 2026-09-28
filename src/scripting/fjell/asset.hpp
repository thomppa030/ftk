// asset.hpp — Asset<T>, a reference to an engine-owned asset
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/api.h"
#include "fjell/missing_reference.hpp"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void* fjell_asset_resolve(uint32_t handle_id, uint8_t asset_type_id);
FJELL_SDK_EXPORT void fjell_asset_missing(const void* asset);
}

namespace fjell {

struct AssetData;
template <typename T>
class Asset;

namespace detail {

/// An Asset holding `data`: how the engine hands a script an asset it names
/// by handle, a weather event starting, say.
template <typename T>
[[nodiscard]] Asset<T> asset_at(const AssetData& data);

} // namespace detail

/// What an Asset holds. The engine reads and writes it in place: it saves the
/// file the handle was loaded from, and sets both when the field is loaded or
/// picked in the inspector.
struct AssetData {
    /// The asset's handle, 0 while unset. A file that failed to load keeps a
    /// handle of its own, so the field still names it.
    uint32_t handle_id{0};
    uint8_t  asset_type_id{0};
    uint8_t  reserved_[3]{};     // keeps sizeof(AssetData) == 8
};

/// A reference to an engine-owned asset: a preset, an audio clip, a VFX
/// effect, an input action, a surface, a material, weather. Declare it as a
/// `[[fj::property]]` and the inspector offers the assets of that kind; the
/// chosen file is saved with the scene.
///
/// An `Asset<Material>` takes any material unless it says which shader types it
/// can use: `[[fj::shader_type(water)]]` (or `(water, fog)`) lists only those
/// materials in the picker and refuses others dropped on the field.
///
/// Scripts use an asset by handing it to the engine: `audio::play(clip, ...)`,
/// `scene::spawn(enemy, ...)`. **Handing over one with nothing there stops the
/// game**, shipped builds included, naming the script, the field and the node:
/// an unset field, or one whose file failed to load. Where leaving it empty is
/// meant, check with `if (asset)` first.
///
/// @code
/// [[fj::property]] fjell::Asset<fjell::Preset> enemy;        // always set
/// [[fj::property]] fjell::Asset<fjell::Audio> spawn_sound;   // optional
///
/// void begin_play() override {
///     fjell::scene::spawn(enemy, {0, 1, 0});
///     if (spawn_sound) fjell::audio::play(spawn_sound, {0, 1, 0});
/// }
/// @endcode
template <typename T>
class Asset {
public:
    Asset() = default;

    /// Whether it holds an asset that is there: false while unset, and for a
    /// file that failed to load.
    [[nodiscard]] explicit operator bool() const {
        return data_.handle_id != 0 && fjell_asset_resolve(data_.handle_id, data_.asset_type_id) != nullptr;
    }

    /// The asset's handle, for the engine calls that take one and for
    /// comparing with the handles the engine reports (a hit's surface, the
    /// weather playing). 0 while unset.
    [[nodiscard]] uint32_t handle_id() const { return data_.handle_id; }

    /// The same asset: the same file, or both unset.
    [[nodiscard]] bool operator==(const Asset& other) const { return data_.handle_id == other.data_.handle_id; }

private:
    friend Asset detail::asset_at<T>(const AssetData& data);

    AssetData data_;
};

namespace detail {

template <typename T>
Asset<T> asset_at(const AssetData& data) {
    Asset<T> asset;
    asset.data_ = data;
    return asset;
}

/// Stops the game unless `asset` holds an asset that is there; what every
/// SDK call that takes one does first. The throw is here, in the game
/// library, rather than in the engine's C export, which a compiler may assume
/// never throws.
template <typename T>
void require(const Asset<T>& asset) {
    if (asset) return;
    fjell_asset_missing(&asset);
    throw MissingReference{};
}

} // namespace detail

} // namespace fjell

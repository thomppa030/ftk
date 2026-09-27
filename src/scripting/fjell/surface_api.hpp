// surface_api.hpp — Physics surfaces for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/asset.hpp"

#include <cstdint>

// ── C ABI ──────────────────────────────────────────────────
extern "C" {
FJELL_SDK_EXPORT const char* fjell_surface_name(uint32_t surface_id);
}

// ── C++ wrapper ────────────────────────────────────────────
namespace fjell {

/// A physics surface, authored as a `.fjsurface`. Hold one as a
/// `[[fj::property]] Asset<Surface>` and compare its handle against what
/// physics reports; the asset's fields are not read from script.
struct Surface;

/// What a collider or terrain layer is made of, as physics reports it: the
/// `surface` of a `PhysicsHit`, a `CharacterComponent::ground_surface`. A
/// surface id is the handle id of the `.fjsurface` asset, so the comparison
/// is an integer compare against an `Asset<Surface>` the script holds.
///
/// @code
/// [[fj::property]] fjell::Asset<fjell::Surface> grass;
/// [[fj::property]] fjell::Asset<fjell::Audio> grass_step;
/// // ...
/// if (auto* c = self().get_component<fjell::CharacterComponent>()) {
///     if (c->ground_surface == grass.handle_id()) {
///         fjell::audio::play(grass_step, transform().position);
///     }
/// }
/// @endcode
namespace surface {
/// Whether `id` names a surface at all. 0 means the collider or layer
/// had none assigned.
inline bool valid(uint32_t id) { return id != 0; }

/// Whether a reported surface is the one an `Asset<Surface>` holds.
inline bool is(uint32_t id, const Asset<Surface>& surface) {
    return id != 0 && id == surface.handle_id();
}

/// The surface's display name, for logging. Empty for 0 or an unknown
/// id. The string is owned by the engine and stays valid while the asset
/// is loaded.
inline const char* name(uint32_t id) {
    const char* n = fjell_surface_name(id);
    return n != nullptr ? n : "";
}
} // namespace surface

} // namespace fjell

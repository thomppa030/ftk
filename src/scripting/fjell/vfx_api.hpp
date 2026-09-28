// vfx_api.hpp — Static VFX API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/asset.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void fjell_vfx_spawn(uint32_t handle_id, float x, float y, float z);
}

namespace fjell {

struct Vfx;

/// Spawns particle effects.
namespace vfx {
/// Spawn a one-shot particle effect at a world position. The effect plays
/// independently of the caller and cleans itself up when finished, so it
/// survives the spawning node being destroyed.
/// @param effect Effect asset, normally a `[[fj::property]] Asset<Vfx>`
///               field assigned in the inspector. One that is unset or
///               failed to load stops the game (see `Asset`).
/// @param pos    World-space spawn position.
/// @code
/// [[fj::property]] fjell::Asset<fjell::Vfx> explosion;
/// // ...
/// fjell::vfx::spawn(explosion, transform().position);
/// @endcode
inline void spawn(const Asset<Vfx>& effect, const glm::vec3& pos) {
    detail::require(effect);
    fjell_vfx_spawn(effect.handle_id(), pos.x, pos.y, pos.z);
}
} // namespace vfx

} // namespace fjell

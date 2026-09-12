// vfx_api.hpp — Static VFX API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void fjell_vfx_spawn(uint32_t handle_id, float x, float y, float z);
}

namespace fjell {

struct VfxAsset;

/// Spawns particle effects.
struct Vfx {
    /// Spawn a one-shot particle effect at a world position. The effect plays
    /// independently of the caller and cleans itself up when finished, so it
    /// survives the spawning node being destroyed.
    /// @param effect Effect asset, normally a `[[fj::property]] Ref<VfxAsset>`
    ///               field assigned in the inspector. Unassigned refs are
    ///               ignored rather than treated as an error.
    /// @param pos    World-space spawn position.
    /// @code
    /// [[fj::property]] fjell::Ref<fjell::VfxAsset> explosion;
    /// // ...
    /// fjell::Vfx::spawn(explosion, transform().position);
    /// @endcode
    static void spawn(const Ref<VfxAsset>& effect, const glm::vec3& pos) {
        if (effect.handle_id() != 0) {
            fjell_vfx_spawn(effect.handle_id(), pos.x, pos.y, pos.z);
        }
    }
};

} // namespace fjell

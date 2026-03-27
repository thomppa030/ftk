// vfx_api.hpp — Static VFX API for game scripts
#pragma once

#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
void fjell_vfx_spawn(uint32_t handle_id, float x, float y, float z);
}

namespace fjell {

struct VfxAsset;

struct Vfx {
    static void spawn(const Ref<VfxAsset>& effect, const glm::vec3& pos) {
        if (effect.handle_id() != 0) {
            fjell_vfx_spawn(effect.handle_id(), pos.x, pos.y, pos.z);
        }
    }
};

} // namespace fjell

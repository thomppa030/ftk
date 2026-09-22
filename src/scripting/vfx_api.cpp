#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/scene.hpp"

extern "C" FJELL_SDK_EXPORT void fjell_vfx_spawn(uint32_t handle_id, float x, float y, float z) {
    auto& s = fjell::script_services();
    if (s.vfx && s.scene && s.resources) {
        s.vfx->spawn(fjell::VfxAssetHandle{handle_id}, {x, y, z}, *s.scene, *s.resources, s.project);
    }
}

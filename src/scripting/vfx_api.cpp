#include "scripting/script_globals.hpp"
#include "renderer/vfx_subsystem.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/resource_handle.hpp"
#include "scene/scene.hpp"

extern "C" void fjell_vfx_spawn(uint32_t handle_id, float x, float y, float z) {
    auto* vfx = fjell::script_vfx_subsystem();
    auto* scene = fjell::script_active_scene();
    auto* rm = fjell::script_resource_manager();
    if (vfx && scene && rm) {
        vfx->spawn(fjell::VfxAssetHandle{handle_id}, {x, y, z}, *scene, *rm);
    }
}

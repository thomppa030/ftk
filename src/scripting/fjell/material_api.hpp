// material_api.hpp — Per-mesh material instance overrides
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void fjell_mesh_set_param_float(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec2 (uint32_t node_id, const char* name, float x, float y);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec3 (uint32_t node_id, const char* name, float x, float y, float z);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec4 (uint32_t node_id, const char* name, float x, float y, float z, float w);
FJELL_SDK_EXPORT void fjell_mesh_reset_material (uint32_t node_id);
}

namespace fjell {

/// Overrides shader parameters on one mesh without affecting others that
/// share the same material.
///
/// Meshes start out sharing a base material. The first `set_param()` on a node
/// clones that base into a private instance for the node; later calls mutate
/// the clone. That clone costs memory and a pipeline lookup, so prefer editing
/// the `.fjmat` when every user of a material should change together.
///
/// Parameter names are the `uniform` names declared in the mesh's `.fjsl`
/// shader. An unknown name is ignored silently, as is a call on a node with
/// no mesh — a typo shows up as a parameter that never changes.
///
/// @code
/// // Flash a hit target red, then restore the shared material.
/// fjell::MaterialAPI::set_param(self().id(), "tint", 1.0f, 0.2f, 0.2f);
/// fjell::Timer::set<&Enemy::clear_flash>(0.1f, this);
/// // void Enemy::clear_flash() { fjell::MaterialAPI::reset(self().id()); }
/// @endcode
struct MaterialAPI {
    /// Set a `float` uniform on this mesh's material instance.
    /// @param node_id Node carrying a MeshComponent.
    /// @param name    Uniform name from the mesh's `.fjsl` shader.
    static void set_param(uint32_t node_id, const char* name, float v) {
        fjell_mesh_set_param_float(node_id, name, v);
    }

    /// Set a `vec2` uniform on this mesh's material instance.
    static void set_param(uint32_t node_id, const char* name, float x, float y) {
        fjell_mesh_set_param_vec2(node_id, name, x, y);
    }

    /// Set a `vec3` uniform — also the form for `hint_color` colors, as RGB.
    static void set_param(uint32_t node_id, const char* name, float x, float y, float z) {
        fjell_mesh_set_param_vec3(node_id, name, x, y, z);
    }

    /// Set a `vec4` uniform — also the form for RGBA colors.
    static void set_param(uint32_t node_id, const char* name,
                          float x, float y, float z, float w) {
        fjell_mesh_set_param_vec4(node_id, name, x, y, z, w);
    }

    /// Discard this mesh's private material instance and return it to the
    /// shared base material, dropping every override at once. Safe to call on
    /// a mesh that was never overridden.
    static void reset(uint32_t node_id) {
        fjell_mesh_reset_material(node_id);
    }
};

} // namespace fjell

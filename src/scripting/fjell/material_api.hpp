// material_api.hpp — Per-mesh material instance overrides
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void fjell_mesh_set_param_float(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec2 (uint32_t node_id, const char* name, float x, float y);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec3 (uint32_t node_id, const char* name, float x, float y, float z);
FJELL_SDK_EXPORT void fjell_mesh_set_param_vec4 (uint32_t node_id, const char* name, float x, float y, float z, float w);
FJELL_SDK_EXPORT void fjell_mesh_reset_material (uint32_t node_id);
}

namespace fjell {

/// Overrides shader parameters on one node's material without affecting
/// others that share it: the material of its mesh, or of its body of water.
///
/// Nodes start out sharing a base material. The first `set_param()` on a node
/// clones that base into a private instance for the node; later calls mutate
/// the clone. That clone costs memory and a pipeline lookup, so prefer editing
/// the `.fjmat` when every user of a material should change together.
///
/// Parameter names are the `uniform` names declared in the material's `.fjsl`
/// shader. An unknown name is ignored silently, as is a node that draws with
/// no material — a typo shows up as a parameter that never changes. A node
/// that is gone stops the game (see `Ref`).
///
/// @code
/// // Flash a hit target red, then restore the shared material.
/// fjell::material::set_param(self(), "tint", 1.0f, 0.2f, 0.2f);
/// fjell::Timer::set<&Enemy::clear_flash>(0.1f, this);
/// // void Enemy::clear_flash() { fjell::material::reset(self()); }
/// @endcode
namespace material {
/// Set a `float` uniform on the node's material instance.
/// @param node The node whose mesh or water to change; `self()` for your own.
/// @param name Uniform name from the material's `.fjsl` shader.
inline void set_param(const Ref<Node3D>& node, const char* name, float v) {
    detail::require(node);
    fjell_mesh_set_param_float(node.node_id(), name, v);
}

/// Set a `vec2` uniform on the node's material instance.
inline void set_param(const Ref<Node3D>& node, const char* name, float x, float y) {
    detail::require(node);
    fjell_mesh_set_param_vec2(node.node_id(), name, x, y);
}

/// Set a `vec3` uniform — also the form for `hint_color` colors, as sRGB
/// RGB exactly as the picker and the `.fjmat` show them; the engine
/// converts to linear on the way to the GPU.
inline void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z) {
    detail::require(node);
    fjell_mesh_set_param_vec3(node.node_id(), name, x, y, z);
}

/// Set a `vec4` uniform — also the form for RGBA colors.
inline void set_param(const Ref<Node3D>& node, const char* name, float x, float y, float z, float w) {
    detail::require(node);
    fjell_mesh_set_param_vec4(node.node_id(), name, x, y, z, w);
}

/// Discard the node's private material instance and return it to the shared
/// base material, dropping every override at once. Safe to call on a node
/// that was never overridden.
inline void reset(const Ref<Node3D>& node) {
    detail::require(node);
    fjell_mesh_reset_material(node.node_id());
}
} // namespace material

} // namespace fjell

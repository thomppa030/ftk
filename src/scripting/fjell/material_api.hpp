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

// Per-mesh material parameter overrides. First call clones the mesh's
// shared base material into a fresh MaterialInstance; later calls mutate
// that instance. reset_material() drops the override and returns the mesh
// to the shared base material.
struct MaterialAPI {
    static void set_param(uint32_t node_id, const char* name, float v) {
        fjell_mesh_set_param_float(node_id, name, v);
    }
    static void set_param(uint32_t node_id, const char* name, float x, float y) {
        fjell_mesh_set_param_vec2(node_id, name, x, y);
    }
    static void set_param(uint32_t node_id, const char* name, float x, float y, float z) {
        fjell_mesh_set_param_vec3(node_id, name, x, y, z);
    }
    static void set_param(uint32_t node_id, const char* name,
                          float x, float y, float z, float w) {
        fjell_mesh_set_param_vec4(node_id, name, x, y, z, w);
    }
    static void reset(uint32_t node_id) {
        fjell_mesh_reset_material(node_id);
    }
};

} // namespace fjell

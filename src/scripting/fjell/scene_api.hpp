// scene_api.hpp — Static scene API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"
#include "fjell/transform.hpp"

#include <cstdint>

// Engine exports (resolved at dlopen via -rdynamic)
extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name);
FJELL_SDK_EXPORT void     fjell_scene_destroy(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_position(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_scale(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_rotation(uint32_t node_id);
FJELL_SDK_EXPORT void*    fjell_scene_get_component(uint32_t node_id, const char* type_name);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_empty(const char* name, float x, float y, float z);
FJELL_SDK_EXPORT void*    fjell_scene_add_component(uint32_t node_id, const char* type_name);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                   float x, float y, float z,
                                                   int32_t parent_id);
}

namespace fjell {

class ScriptComponent;
struct PresetAsset;

struct SceneAPI {
    static uint32_t find_node(const char* name) {
        return fjell_scene_find_node(name);
    }

    /// Spawn an empty node at a position.
    static uint32_t spawn(const char* name,
                          const glm::vec3& pos = glm::vec3{0.0f}) {
        return fjell_scene_spawn_empty(name, pos.x, pos.y, pos.z);
    }

    /// Spawn a preset (.fjp) at a position. Returns root node ID (0 on failure).
    static uint32_t spawn(const Ref<PresetAsset>& preset,
                          const glm::vec3& pos = glm::vec3{0.0f},
                          int32_t parent_id = -1) {
        if (!preset.valid()) return 0;
        return fjell_scene_spawn_preset(preset.handle_id(),
                                        pos.x, pos.y, pos.z, parent_id);
    }
};

} // namespace fjell

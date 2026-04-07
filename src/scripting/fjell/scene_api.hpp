// scene_api.hpp — Static scene API for game scripts
#pragma once

#include "fjell/api.h"
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
}

namespace fjell {

class ScriptComponent;

struct SceneAPI {
    static uint32_t find_node(const char* name) {
        return fjell_scene_find_node(name);
    }

    static uint32_t spawn_empty(const char* name,
                                const glm::vec3& pos = glm::vec3{0.0f}) {
        return fjell_scene_spawn_empty(name, pos.x, pos.y, pos.z);
    }
};

} // namespace fjell

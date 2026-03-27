// node.hpp — Lightweight node handle for game scripts
#pragma once

#include "fjell/component.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

extern "C" {
void     fjell_scene_destroy(uint32_t node_id);
float*   fjell_scene_get_position(uint32_t node_id);
float*   fjell_scene_get_scale(uint32_t node_id);
float*   fjell_scene_get_rotation(uint32_t node_id);
void*    fjell_scene_get_component(uint32_t node_id, const char* type_name);
void*    fjell_scene_add_component(uint32_t node_id, const char* type_name);
}

namespace fjell {

struct Node3D {
    uint32_t node_id{0};

    Node3D() = default;
    explicit Node3D(uint32_t id) : node_id{id} {}

    [[nodiscard]] uint32_t id() const { return node_id; }
    [[nodiscard]] bool valid() const { return node_id != 0; }

    [[nodiscard]] glm::vec3& position() const {
        return *reinterpret_cast<glm::vec3*>(fjell_scene_get_position(node_id));
    }

    [[nodiscard]] glm::vec3& scale() const {
        return *reinterpret_cast<glm::vec3*>(fjell_scene_get_scale(node_id));
    }

    [[nodiscard]] glm::quat& rotation() const {
        return *reinterpret_cast<glm::quat*>(fjell_scene_get_rotation(node_id));
    }

    template <typename T>
    [[nodiscard]] T* get_component() const {
        return static_cast<T*>(fjell_scene_get_component(node_id, T::type_name));
    }

    template <typename T>
    T* add_component() const {
        return static_cast<T*>(fjell_scene_add_component(node_id, T::type_name));
    }

    void destroy() const { fjell_scene_destroy(node_id); }
};

// Deferred definition — needs Node3D to be complete
inline Node3D ScriptComponent::self() const { return Node3D{id_}; }

} // namespace fjell

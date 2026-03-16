#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

enum class BodyType : uint8_t { Static, Kinematic, Dynamic };
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Mesh };

struct RigidbodyDesc {
    BodyType body_type{BodyType::Dynamic};
    float mass{1.0f};
    float friction{0.5f};
    float restitution{0.3f};
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    bool gravity_enabled{true};
};

struct ColliderDesc {
    ColliderShape shape{ColliderShape::Box};
    glm::vec3 half_extents{0.5f};
    float radius{0.5f};
    float height{1.0f};
    glm::vec3 offset{0.0f};
};

struct RaycastHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    float distance{0.0f};
    uint32_t object_id{0};
};

} // namespace fjell

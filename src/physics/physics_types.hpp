#pragma once

#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

enum class BodyType : uint8_t { Static, Kinematic, Dynamic };
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Mesh };

struct RigidbodyComponent : NodeComponent {
    BodyType body_type{BodyType::Dynamic};
    float mass{1.0f};
    float friction{0.5f};
    float restitution{0.3f};
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    bool gravity_enabled{true};
};

struct ColliderComponent : NodeComponent {
    /// Use the collision setup authored on the node's mesh instead of the
    /// fields below. Set collision up once on the asset rather than on every
    /// object placed from it. Ignored when the node has no mesh.
    bool inherit_from_mesh{false};

    ColliderShape shape{ColliderShape::Box};
    glm::vec3 half_extents{0.5f};
    float radius{0.5f};
    float height{1.0f};
    glm::vec3 offset{0.0f};
    bool is_trigger{false};
};

enum class CharacterGroundState : uint8_t { OnGround, OnSteepGround, NotSupported, InAir };

struct CharacterComponent : NodeComponent {
    float capsule_radius{0.3f};
    float capsule_height{1.4f};
    float max_slope_angle{50.0f};
    float mass{80.0f};

    // Runtime (not serialized, updated by physics each frame)
    CharacterGroundState ground_state{CharacterGroundState::InAir};
    glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
    glm::vec3 ground_velocity{0.0f};
    glm::vec3 linear_velocity{0.0f};
};

struct CollisionEvent {
    uint32_t other_id{0};
    glm::vec3 contact_point{0.0f};
    glm::vec3 contact_normal{0.0f};
};

struct RaycastHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    float distance{0.0f};
    uint32_t object_id{0};
};

} // namespace fjell

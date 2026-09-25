#include "physics/physics_types.hpp"
#include "physics/collision_shape.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/model.hpp"
#include "scene/node.hpp"

#include <glm/glm.hpp>

namespace fjell {

namespace {

/// `from` moved toward `to` by at most `max_change`.
[[nodiscard]] glm::vec2 move_toward(glm::vec2 from, glm::vec2 to, float max_change) {
    const glm::vec2 gap = to - from;
    const float distance = glm::length(gap);
    if (distance <= max_change || distance < 1e-6f) return to;
    return from + gap * (max_change / distance);
}

} // namespace

void steer_by_intent(CharacterComponent& character, float dt) {
    glm::vec2 intent{character.move_intent.x, character.move_intent.z};
    const float amount = glm::length(intent);
    if (amount > 1.0f) intent /= amount;
    const glm::vec2 intended = intent * character.max_speed;

    glm::vec2 horizontal{character.velocity.x, character.velocity.z};
    const bool grounded = character.ground_state == CharacterGroundState::OnGround;
    if (grounded) {
        horizontal = character.ground_acceleration > 0.0f
                         ? move_toward(horizontal, intended, character.ground_acceleration * dt)
                         : intended;
    } else if (amount > 0.0f) {
        horizontal = move_toward(horizontal, intended,
                                 character.air_acceleration * character.air_control * dt);
    }
    character.velocity.x = horizontal.x;
    character.velocity.z = horizontal.y;

    if (character.jump_requested) {
        if (grounded) character.velocity.y = character.jump_speed;
        character.jump_requested = false;
    }
}

ColliderShape effective_collider_shape(const ColliderComponent& collider, const Node& node) {
    if (!collider.inherit_from_mesh) return collider.shape;
    const auto* mesh = node.components.get<MeshComponent>();
    if (mesh && mesh->model && collision_mesh_of(*mesh->model).valid()) {
        return ColliderShape::Mesh;
    }
    return collider.shape;
}

bool fit_collider_to_primitive(ColliderComponent& collider, const std::string& source) {
    if (source == "primitive:cube") {
        collider.shape = ColliderShape::Box;
        collider.half_extents = glm::vec3{0.5F};
        return true;
    }
    if (source == "primitive:sphere") {
        collider.shape = ColliderShape::Sphere;
        collider.radius = 0.5F;
        return true;
    }
    if (source == "primitive:plane") {
        // A plane has no thickness, and a zero-height box is not a shape any
        // backend will build. 5cm is the thinnest a box can be before the
        // backend's convex radius exceeds its half-extent, and thin enough to
        // sit visually flush under whatever rests on it.
        collider.shape = ColliderShape::Box;
        collider.half_extents = {0.5F, 0.05F, 0.5F};
        return true;
    }
    return false;
}

} // namespace fjell

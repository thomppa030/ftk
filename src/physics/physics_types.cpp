#include "physics/physics_types.hpp"

namespace fjell {

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

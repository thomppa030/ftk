#include "physics/buoyancy.hpp"

#include "physics/physics_types.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

namespace {

/// Eight points spread to the box's corners horizontally, in two layers.
///
/// Full width, because that is what makes a crate tip over an edge rather
/// than pivot about its middle. Half height, because each point stands for a
/// slab of the body and a slab is weighed at its middle — points on the top
/// and bottom faces would have the body behave as though all of it were
/// gathered at its skin, and the lift then runs out before a dense hull
/// reaches its waterline.
void box_points(const glm::vec3& half, const glm::vec3& offset,
                std::vector<glm::vec3>& out) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(offset + glm::vec3((i & 1) ? half.x : -half.x,
                                         (i & 2) ? half.y * 0.5f : -half.y * 0.5f,
                                         (i & 4) ? half.z : -half.z));
    }
}

/// Six points on the axes. A round body has no corners to catch, so its
/// extents are all the shape there is to sample. The vertical pair sits at
/// half height for the same reason the box's layers do.
void axis_points(const glm::vec3& half, const glm::vec3& offset,
                 std::vector<glm::vec3>& out) {
    out.push_back(offset + glm::vec3(half.x, 0.0f, 0.0f));
    out.push_back(offset - glm::vec3(half.x, 0.0f, 0.0f));
    out.push_back(offset + glm::vec3(0.0f, half.y * 0.5f, 0.0f));
    out.push_back(offset - glm::vec3(0.0f, half.y * 0.5f, 0.0f));
    out.push_back(offset + glm::vec3(0.0f, 0.0f, half.z));
    out.push_back(offset - glm::vec3(0.0f, 0.0f, half.z));
}

} // namespace

BuoyancySamples buoyancy_samples(const BuoyancyComponent& buoyancy,
                                 const ColliderComponent* collider) {
    BuoyancySamples samples;

    // No collider is no size to displace water with. A body can still float
    // on authored points, but it needs a volume from somewhere, and guessing
    // one would float it at a scale nothing in the scene agrees with.
    if (collider == nullptr) return samples;

    glm::vec3 half{0.0f};
    switch (collider->shape) {
    case ColliderShape::Box:
        half = collider->half_extents;
        samples.volume = 8.0f * half.x * half.y * half.z;
        break;
    case ColliderShape::Sphere:
        half = glm::vec3(collider->radius);
        samples.volume = (4.0f / 3.0f) * 3.14159265f * collider->radius * collider->radius
                       * collider->radius;
        break;
    case ColliderShape::Capsule: {
        const float r = collider->radius;
        half = glm::vec3(r, collider->height * 0.5f + r, r);
        // A cylinder between two hemispheres, which together are a sphere.
        samples.volume = 3.14159265f * r * r * collider->height
                       + (4.0f / 3.0f) * 3.14159265f * r * r * r;
        break;
    }
    case ColliderShape::Mesh:
        // Reading a hull's triangles every frame buys very little over the
        // box it sits in, and the box is what the collider already carries.
        half = collider->half_extents;
        samples.volume = 8.0f * half.x * half.y * half.z;
        break;
    }

    // The slab one layer of points stands for. The derived points sit in two
    // layers at half height, so each owns half the body and fades in over
    // that — which is what puts a hull's waterline where its density says
    // rather than wherever its skin happens to cross the surface.
    samples.span = std::max(half.y, 1e-3f);

    if (!buoyancy.points.empty()) {
        samples.points = buoyancy.points;
        return samples;
    }

    switch (collider->shape) {
    case ColliderShape::Box:
    case ColliderShape::Mesh:
        box_points(half, collider->offset, samples.points);
        break;
    case ColliderShape::Sphere:
    case ColliderShape::Capsule:
        axis_points(half, collider->offset, samples.points);
        break;
    }
    return samples;
}

BuoyancyForce buoyancy_at(float depth, float point_volume, float span, float drag,
                          const glm::vec3& point_velocity, const glm::vec3& flow,
                          float gravity) {
    BuoyancyForce result;
    if (point_volume <= 0.0f) return result;

    // How much of this point's share is under water. The point stands for a
    // slab of the body centred on it, so the slab is half under when the
    // point itself is at the surface, and fully under half a span below —
    // not a whole span below, which would have a hull ride a half-span too
    // deep and a light one never surface at all.
    const float thickness = std::max(span, 1e-3f);
    const float fraction = std::clamp((depth + thickness * 0.5f) / thickness, 0.0f, 1.0f);
    if (fraction <= 0.0f) return result;

    result.submerged = true;
    const float displaced = point_volume * fraction;

    // The mass of the water this point has pushed aside. Density matters
    // here and only here: the lift is what that water weighs, and the body's
    // own density never enters — it set the body's mass, and mass against
    // this lift is what picks the waterline.
    const float displaced_mass = displaced * WATER_DENSITY;

    // Archimedes: up by the weight of the water displaced, and nothing else.
    // The body's own weight is gravity's to apply and the solver already
    // does; subtracting it again here would have every float carrying its
    // weight twice and a crate of any density would sink.
    //
    // Density is therefore not in this force at all. It decides the body's
    // mass, and the balance between that mass and this lift is what settles a
    // hull at its waterline: something 0.6 as dense as water floats with 60%
    // of itself under, which is where the lift it gets there equals what it
    // weighs.
    result.force.y += displaced_mass * gravity;

    // Drag against the water's own motion rather than against the world, so a
    // body sitting in a current is carried by it instead of being dragged
    // through it. Without this a float dropped in water would oscillate on
    // its own buoyancy forever, because nothing takes the energy back out.
    const glm::vec3 relative = point_velocity - flow;
    result.force -= relative * (drag * displaced_mass);
    return result;
}

} // namespace fjell

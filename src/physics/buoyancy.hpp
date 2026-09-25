#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

struct BuoyancyComponent;
struct ColliderComponent;

/// The density of fresh water, in kilograms per cubic metre.
///
/// `BuoyancyComponent::density` is quoted against this, so a body's mass is
/// its volume times its density times this number — a cubic metre of
/// something half as dense as water weighs 500 kg.
inline constexpr float WATER_DENSITY = 1000.0f;

/// Where a body is sampled against the water, and what each point is worth.
///
/// The points are in the node's local space and the volume is the body's
/// total, shared equally between them: each point carries `volume / count`,
/// so a body samples the same displacement however finely it is sampled.
/// Doubling the points doubles the resolution without doubling the lift.
struct BuoyancySamples {
    std::vector<glm::vec3> points;
    float volume{0.0f};

    /// How deep each point's share is spread, so a point crossing the surface
    /// fades rather than switching its whole share on in one frame.
    ///
    /// This is the body's own height rather than a fixed distance: a point on
    /// a barge stands for a much thicker slice than one on a crate, and a
    /// constant would make a big hull saturate almost the moment it touched
    /// the water and settle far too high.
    float span{1.0f};
};

/// The points to sample a body at, from its collider, or from the component's
/// own list when it has one.
///
/// A box gives its eight corners, so a crate rolls over when it is tipped. A
/// sphere and a capsule give six points on their axes, which is enough for
/// something that has no edges to catch. A mesh collider falls back to the
/// bounds it was given, because reading a hull's triangles every frame is a
/// cost that buys very little over its box.
[[nodiscard]] BuoyancySamples buoyancy_samples(const BuoyancyComponent& buoyancy,
                                               const ColliderComponent* collider);

/// The same, written into `samples` over whatever it held, so a caller
/// sampling every fixed step reuses the storage instead of allocating it.
void buoyancy_samples(const BuoyancyComponent& buoyancy, const ColliderComponent* collider,
                      BuoyancySamples& samples);

/// What one sample point contributes, in world space.
///
/// `depth` is how far the point sits below the surface; at or above it there
/// is no force. The lift is the weight of the water displaced and nothing
/// else — the body's own weight is gravity's to apply, and the solver already
/// does. Subtracting it here too would have every float carry its weight
/// twice and sink whatever its density.
///
/// That also makes this Archimedes rather than a spring: the lift stops at
/// the point's full share of the volume rather than growing with depth, so a
/// body held under is not fired out when released. Where a hull settles is
/// the balance between this lift and its mass — something 0.6 as dense as
/// water floats with 60% of itself under.
///
/// `span` is how deep this point's share is spread, which is what stops a
/// point being all-or-nothing as it crosses the surface. A body's points each
/// stand for a slice of it, and a slice goes under gradually.
///
/// `flow` is the water's own motion at that point, so a body is dragged
/// against the water rather than against the world and drifts with a current
/// instead of standing still in it.
struct BuoyancyForce {
    glm::vec3 force{0.0f};
    bool submerged{false};
};

[[nodiscard]] BuoyancyForce buoyancy_at(float depth, float point_volume, float span,
                                        float drag, const glm::vec3& point_velocity,
                                        const glm::vec3& flow, float gravity);

/// How deep a point is, given the surface height above it. Positive is under.
[[nodiscard]] constexpr float submersion_depth(float point_y, float surface_y) {
    return surface_y - point_y;
}

} // namespace fjell

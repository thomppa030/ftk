// physics_api.hpp — Physics queries and the world's gravity, for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/// Collision channels. A collider belongs to one and decides what it does
/// about every other; queries use them to say what they care about.
enum FjellCollisionChannel : uint32_t {
    FJ_CHANNEL_WORLD_STATIC  = 0,
    FJ_CHANNEL_WORLD_DYNAMIC = 1,
    FJ_CHANNEL_PAWN          = 2,
    FJ_CHANNEL_TRIGGER       = 3,
    FJ_CHANNEL_PROJECTILE    = 4,
};

/// Channel bits for a query's mask. OR them together.
enum FjellChannelMask : uint32_t {
    FJ_MASK_WORLD_STATIC  = 1U << FJ_CHANNEL_WORLD_STATIC,
    FJ_MASK_WORLD_DYNAMIC = 1U << FJ_CHANNEL_WORLD_DYNAMIC,
    FJ_MASK_PAWN          = 1U << FJ_CHANNEL_PAWN,
    FJ_MASK_TRIGGER       = 1U << FJ_CHANNEL_TRIGGER,
    FJ_MASK_PROJECTILE    = 1U << FJ_CHANNEL_PROJECTILE,
    FJ_MASK_WORLD         = FJ_MASK_WORLD_STATIC | FJ_MASK_WORLD_DYNAMIC,
    FJ_MASK_ALL           = 0xFFFFFFFF,
};

/// What a query swept or tested against.
struct FjellRaycastHit {
    float position[3];
    float normal[3];
    float distance;
    uint32_t node_id;
    uint32_t surface;
};

extern "C" {
FJELL_SDK_EXPORT int      fjell_physics_raycast(const float* origin, const float* dir,
                                                float max_distance, uint32_t channel_mask,
                                                uint32_t ignore_node, FjellRaycastHit* out);
FJELL_SDK_EXPORT uint32_t fjell_physics_raycast_all(const float* origin, const float* dir,
                                                    float max_distance, uint32_t channel_mask,
                                                    uint32_t ignore_node,
                                                    FjellRaycastHit* out, uint32_t max_hits);
FJELL_SDK_EXPORT int      fjell_physics_shape_cast(uint32_t shape, const float* extents,
                                                   const float* origin, const float* dir,
                                                   float max_distance, uint32_t channel_mask,
                                                   uint32_t ignore_node, FjellRaycastHit* out);
FJELL_SDK_EXPORT uint32_t fjell_physics_overlap(uint32_t shape, const float* extents,
                                                const float* position, uint32_t channel_mask,
                                                uint32_t ignore_node,
                                                uint32_t* out_nodes, uint32_t max_nodes);
FJELL_SDK_EXPORT void     fjell_physics_set_gravity(const float* gravity);
FJELL_SDK_EXPORT void     fjell_physics_get_gravity(float* out);
}

namespace fjell {

/// A hit from a physics query.
struct PhysicsHit {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F};
    float distance{0.0F};
    /// The node whose collider was hit.
    Ref<Node3D> node;
    /// Surface id of what was hit — the handle id of the `.fjsurface` on the
    /// collider or terrain layer, 0 for none. See `surface`.
    uint32_t surface{0};
};

/// Narrows what a query looks at.
///
/// `ignore` is the node the query came from, which would otherwise be the
/// first thing a cast started inside its own collider reports: `self()`
/// passes a script's own.
struct PhysicsQuery {
    uint32_t channels{FJ_MASK_ALL};
    Ref<Node3D> ignore;
};

/// Queries against the running physics world, and its gravity. A body is
/// pushed and its velocity read through its `RigidbodyComponent`.
///
/// Every call is inert outside play mode — there is no physics world in the
/// editor — so a query returns no hit rather than misbehaving.
///
/// @code
/// // What is the player looking at, ignoring triggers and itself?
/// fjell::PhysicsHit hit;
/// if (fjell::physics::raycast(eye, forward, 100.0F, hit, {FJ_MASK_WORLD | FJ_MASK_PAWN, self()})) {
///     if (auto door = hit.node->get_component<Door>()) door->open();
/// }
/// @endcode
namespace physics {
// What the functions below share, not for scripts to call.
namespace detail {

inline constexpr uint32_t SHAPE_SPHERE = 0;
inline constexpr uint32_t SHAPE_BOX = 1;
inline constexpr uint32_t SHAPE_CAPSULE = 2;

// A ray through a crowd rarely wants more than this, and a fixed buffer
// keeps the C boundary free of allocation.
inline constexpr uint32_t MAX_HITS = 64;

inline PhysicsHit to_hit(const FjellRaycastHit& raw) {
    return {{raw.position[0], raw.position[1], raw.position[2]},
            {raw.normal[0], raw.normal[1], raw.normal[2]},
            raw.distance, fjell::detail::ref_to<Node3D>(raw.node_id), raw.surface};
}

inline bool cast(uint32_t shape, const glm::vec3& extents, const glm::vec3& origin,
                 const glm::vec3& direction, float max_distance, PhysicsHit& out,
                 const PhysicsQuery& query) {
    FjellRaycastHit hit{};
    if (fjell_physics_shape_cast(shape, &extents.x, &origin.x, &direction.x,
                                 max_distance, query.channels, query.ignore.node_id(),
                                 &hit) == 0) {
        return false;
    }
    out = to_hit(hit);
    return true;
}

inline std::vector<Ref<Node3D>> overlap(uint32_t shape, const glm::vec3& extents,
                                        const glm::vec3& position,
                                        const PhysicsQuery& query) {
    uint32_t raw[MAX_HITS];
    uint32_t count = fjell_physics_overlap(shape, &extents.x, &position.x,
                                           query.channels, query.ignore.node_id(),
                                           raw, MAX_HITS);
    std::vector<Ref<Node3D>> nodes;
    nodes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) nodes.push_back(fjell::detail::ref_to<Node3D>(raw[i]));
    return nodes;
}

} // namespace detail

/// Nearest hit along a ray. Returns false when nothing is hit.
inline bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                    float max_distance, PhysicsHit& out,
                    const PhysicsQuery& query = {}) {
    FjellRaycastHit hit{};
    if (fjell_physics_raycast(&origin.x, &direction.x, max_distance,
                              query.channels, query.ignore.node_id(), &hit) == 0) {
        return false;
    }
    out = detail::to_hit(hit);
    return true;
}

/// Every hit along a ray, nearest first.
inline std::vector<PhysicsHit> raycast_all(const glm::vec3& origin,
                                           const glm::vec3& direction,
                                           float max_distance,
                                           const PhysicsQuery& query = {}) {
    FjellRaycastHit raw[detail::MAX_HITS];
    uint32_t count = fjell_physics_raycast_all(&origin.x, &direction.x, max_distance,
                                               query.channels, query.ignore.node_id(),
                                               raw, detail::MAX_HITS);
    std::vector<PhysicsHit> hits;
    hits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) hits.push_back(detail::to_hit(raw[i]));
    return hits;
}

/// Sweeps a sphere and reports the first thing it touches. Prefer this over
/// a ray for anything that has width — a ray slips through gaps a character
/// cannot.
inline bool sphere_cast(const glm::vec3& origin, float radius,
                        const glm::vec3& direction, float max_distance,
                        PhysicsHit& out, const PhysicsQuery& query = {}) {
    return detail::cast(detail::SHAPE_SPHERE, {radius, 0.0F, 0.0F}, origin, direction,
                max_distance, out, query);
}

inline bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents,
                     const glm::vec3& direction, float max_distance,
                     PhysicsHit& out, const PhysicsQuery& query = {}) {
    return detail::cast(detail::SHAPE_BOX, half_extents, origin, direction, max_distance, out, query);
}

inline bool capsule_cast(const glm::vec3& origin, float radius, float height,
                         const glm::vec3& direction, float max_distance,
                         PhysicsHit& out, const PhysicsQuery& query = {}) {
    return detail::cast(detail::SHAPE_CAPSULE, {radius, height, 0.0F}, origin, direction,
                max_distance, out, query);
}

/// Nodes whose colliders intersect a sphere at `center`. Unordered.
inline std::vector<Ref<Node3D>> overlap_sphere(const glm::vec3& center, float radius,
                                            const PhysicsQuery& query = {}) {
    return detail::overlap(detail::SHAPE_SPHERE, {radius, 0.0F, 0.0F}, center, query);
}

/// Nodes whose colliders intersect a box at `center`. Unordered.
inline std::vector<Ref<Node3D>> overlap_box(const glm::vec3& center,
                                            const glm::vec3& half_extents,
                                            const PhysicsQuery& query = {}) {
    return detail::overlap(detail::SHAPE_BOX, half_extents, center, query);
}

/// The gravity of the whole physics world, in metres per second squared.
inline void set_gravity(const glm::vec3& gravity) {
    fjell_physics_set_gravity(&gravity.x);
}

[[nodiscard]] inline glm::vec3 gravity() {
    glm::vec3 g{0.0F};
    fjell_physics_get_gravity(&g.x);
    return g;
}
} // namespace physics

} // namespace fjell

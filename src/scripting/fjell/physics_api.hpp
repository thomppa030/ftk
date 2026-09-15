// physics_api.hpp — Static physics API for game scripts
#pragma once

#include "fjell/api.h"

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
FJELL_SDK_EXPORT void     fjell_physics_apply_force(uint32_t node_id, const float* force);
FJELL_SDK_EXPORT void     fjell_physics_apply_impulse(uint32_t node_id, const float* impulse);
FJELL_SDK_EXPORT void     fjell_physics_set_linear_velocity(uint32_t node_id, const float* v);
FJELL_SDK_EXPORT void     fjell_physics_get_linear_velocity(uint32_t node_id, float* out);
FJELL_SDK_EXPORT void     fjell_physics_set_angular_velocity(uint32_t node_id, const float* v);
FJELL_SDK_EXPORT void     fjell_physics_get_angular_velocity(uint32_t node_id, float* out);
FJELL_SDK_EXPORT void     fjell_physics_set_gravity(const float* gravity);
FJELL_SDK_EXPORT void     fjell_physics_get_gravity(float* out);
}

namespace fjell {

/// A hit from a physics query.
struct PhysicsHit {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F};
    float distance{0.0F};
    uint32_t node_id{0};
    /// Surface id of what was hit — the handle id of the `.fjsurface` on the
    /// collider or terrain layer, 0 for none. See `Surface`.
    uint32_t surface{0};
};

/// Narrows what a query looks at.
///
/// `ignore` is the node the query came from, which would otherwise be the
/// first thing a cast started inside its own collider reports.
struct PhysicsQuery {
    uint32_t channels{FJ_MASK_ALL};
    uint32_t ignore{0};
};

/// Queries and forces against the running physics world.
///
/// Every call is inert outside play mode — there is no physics world in the
/// editor — so a query returns no hit rather than misbehaving.
///
/// @code
/// // What is the player looking at, ignoring triggers and itself?
/// fjell::PhysicsHit hit;
/// if (fjell::PhysicsAPI::raycast(eye, forward, 100.0F, hit,
///                                {FJ_MASK_WORLD | FJ_MASK_PAWN, node_id()})) {
///     log("looking at node {}", hit.node_id);
/// }
/// @endcode
struct PhysicsAPI {
    /// Nearest hit along a ray. Returns false when nothing is hit.
    static bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                        float max_distance, PhysicsHit& out,
                        const PhysicsQuery& query = {}) {
        FjellRaycastHit hit{};
        if (fjell_physics_raycast(&origin.x, &direction.x, max_distance,
                                  query.channels, query.ignore, &hit) == 0) {
            return false;
        }
        out = to_hit(hit);
        return true;
    }

    /// Every hit along a ray, nearest first.
    static std::vector<PhysicsHit> raycast_all(const glm::vec3& origin,
                                               const glm::vec3& direction,
                                               float max_distance,
                                               const PhysicsQuery& query = {}) {
        FjellRaycastHit raw[MAX_HITS];
        uint32_t count = fjell_physics_raycast_all(&origin.x, &direction.x, max_distance,
                                                   query.channels, query.ignore,
                                                   raw, MAX_HITS);
        std::vector<PhysicsHit> hits;
        hits.reserve(count);
        for (uint32_t i = 0; i < count; ++i) hits.push_back(to_hit(raw[i]));
        return hits;
    }

    /// Sweeps a sphere and reports the first thing it touches. Prefer this over
    /// a ray for anything that has width — a ray slips through gaps a character
    /// cannot.
    static bool sphere_cast(const glm::vec3& origin, float radius,
                            const glm::vec3& direction, float max_distance,
                            PhysicsHit& out, const PhysicsQuery& query = {}) {
        return cast(SHAPE_SPHERE, {radius, 0.0F, 0.0F}, origin, direction,
                    max_distance, out, query);
    }

    static bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents,
                         const glm::vec3& direction, float max_distance,
                         PhysicsHit& out, const PhysicsQuery& query = {}) {
        return cast(SHAPE_BOX, half_extents, origin, direction, max_distance, out, query);
    }

    static bool capsule_cast(const glm::vec3& origin, float radius, float height,
                             const glm::vec3& direction, float max_distance,
                             PhysicsHit& out, const PhysicsQuery& query = {}) {
        return cast(SHAPE_CAPSULE, {radius, height, 0.0F}, origin, direction,
                    max_distance, out, query);
    }

    /// Nodes whose colliders intersect a sphere at `center`. Unordered.
    static std::vector<uint32_t> overlap_sphere(const glm::vec3& center, float radius,
                                                const PhysicsQuery& query = {}) {
        return overlap(SHAPE_SPHERE, {radius, 0.0F, 0.0F}, center, query);
    }

    static std::vector<uint32_t> overlap_box(const glm::vec3& center,
                                             const glm::vec3& half_extents,
                                             const PhysicsQuery& query = {}) {
        return overlap(SHAPE_BOX, half_extents, center, query);
    }

    /// Continuous push, applied over the step. For an instant change in
    /// momentum — a jump, an explosion — use apply_impulse.
    static void apply_force(uint32_t node_id, const glm::vec3& force) {
        fjell_physics_apply_force(node_id, &force.x);
    }

    static void apply_impulse(uint32_t node_id, const glm::vec3& impulse) {
        fjell_physics_apply_impulse(node_id, &impulse.x);
    }

    static void set_linear_velocity(uint32_t node_id, const glm::vec3& velocity) {
        fjell_physics_set_linear_velocity(node_id, &velocity.x);
    }

    [[nodiscard]] static glm::vec3 linear_velocity(uint32_t node_id) {
        glm::vec3 v{0.0F};
        fjell_physics_get_linear_velocity(node_id, &v.x);
        return v;
    }

    static void set_angular_velocity(uint32_t node_id, const glm::vec3& velocity) {
        fjell_physics_set_angular_velocity(node_id, &velocity.x);
    }

    [[nodiscard]] static glm::vec3 angular_velocity(uint32_t node_id) {
        glm::vec3 v{0.0F};
        fjell_physics_get_angular_velocity(node_id, &v.x);
        return v;
    }

    static void set_gravity(const glm::vec3& gravity) {
        fjell_physics_set_gravity(&gravity.x);
    }

    [[nodiscard]] static glm::vec3 gravity() {
        glm::vec3 g{0.0F};
        fjell_physics_get_gravity(&g.x);
        return g;
    }

private:
    static constexpr uint32_t SHAPE_SPHERE = 0;
    static constexpr uint32_t SHAPE_BOX = 1;
    static constexpr uint32_t SHAPE_CAPSULE = 2;

    // A ray through a crowd rarely wants more than this, and a fixed buffer
    // keeps the C boundary free of allocation.
    static constexpr uint32_t MAX_HITS = 64;

    static PhysicsHit to_hit(const FjellRaycastHit& raw) {
        return {{raw.position[0], raw.position[1], raw.position[2]},
                {raw.normal[0], raw.normal[1], raw.normal[2]},
                raw.distance, raw.node_id, raw.surface};
    }

    static bool cast(uint32_t shape, const glm::vec3& extents, const glm::vec3& origin,
                     const glm::vec3& direction, float max_distance, PhysicsHit& out,
                     const PhysicsQuery& query) {
        FjellRaycastHit hit{};
        if (fjell_physics_shape_cast(shape, &extents.x, &origin.x, &direction.x,
                                     max_distance, query.channels, query.ignore,
                                     &hit) == 0) {
            return false;
        }
        out = to_hit(hit);
        return true;
    }

    static std::vector<uint32_t> overlap(uint32_t shape, const glm::vec3& extents,
                                         const glm::vec3& position,
                                         const PhysicsQuery& query) {
        uint32_t raw[MAX_HITS];
        uint32_t count = fjell_physics_overlap(shape, &extents.x, &position.x,
                                               query.channels, query.ignore,
                                               raw, MAX_HITS);
        return {raw, raw + count};
    }
};

} // namespace fjell

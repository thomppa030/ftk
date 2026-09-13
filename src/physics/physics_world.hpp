#pragma once

#include "physics/physics_types.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fjell {

class Scene;
class Model;

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    void create_bodies(Scene& scene);
    void step(float dt, Scene& scene);
    void remove_body(uint32_t object_id);

    void apply_force(uint32_t object_id, const glm::vec3& force);
    void apply_impulse(uint32_t object_id, const glm::vec3& impulse);
    void set_linear_velocity(uint32_t object_id, const glm::vec3& vel);
    [[nodiscard]] glm::vec3 get_linear_velocity(uint32_t object_id) const;
    void set_angular_velocity(uint32_t object_id, const glm::vec3& vel);
    [[nodiscard]] glm::vec3 get_angular_velocity(uint32_t object_id) const;

    /// Nearest hit along a ray. `filter` narrows which bodies are eligible.
    [[nodiscard]] bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                               float max_distance, RaycastHit& out_hit,
                               const QueryFilter& filter = {}) const;

    /// Every hit along a ray, nearest first. Use when a shot should pass
    /// through several targets, or to find what is behind the first wall.
    [[nodiscard]] std::vector<RaycastHit> raycast_all(
        const glm::vec3& origin, const glm::vec3& direction, float max_distance,
        const QueryFilter& filter = {}) const;

    /// Sweeps a shape along a direction and reports the first thing it hits.
    /// A ray finds a gap a character cannot fit through; a swept capsule does
    /// not, which is why movement code wants this rather than raycast().
    /// `extents` is the box half-extents, or x = radius and y = height for a
    /// sphere and capsule.
    [[nodiscard]] bool shape_cast(QueryShape shape, const glm::vec3& extents,
                                  const glm::vec3& origin, const glm::vec3& direction,
                                  float max_distance, RaycastHit& out_hit,
                                  const QueryFilter& filter = {}) const;

    /// Every node whose collider intersects the shape at `position`. The
    /// answer is unordered — an overlap has no nearest.
    [[nodiscard]] std::vector<uint32_t> overlap(QueryShape shape, const glm::vec3& extents,
                                                const glm::vec3& position,
                                                const QueryFilter& filter = {}) const;

    void set_gravity(const glm::vec3& gravity);
    [[nodiscard]] glm::vec3 gravity() const;

    /// Collision/trigger events collected during the last step.
    /// Each pair appears once. Cleared at the start of each step.
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& collision_enter_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& collision_exit_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& trigger_enter_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& trigger_exit_events() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell

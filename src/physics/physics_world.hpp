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

    [[nodiscard]] bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                               float max_distance, RaycastHit& out_hit) const;

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

#pragma once

#include "physics/physics_types.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fjell {

class Model;
struct Node;
struct Node3D;
class Scene;

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    /// Creates bodies for every node in the scene. Called once when play
    /// starts; a node added later goes through create_body instead.
    void create_bodies(Scene& scene);

    /// Creates the body for one node, replacing any it already has. Safe to
    /// call on a node that should not have one — it simply makes none. The
    /// scene is needed because a node's placement depends on its parents.
    void create_body(Scene& scene, Node& node);

    /// Destroys a node's body. A no-op when it has none.
    void destroy_body(uint32_t object_id);

    void step(float dt, Scene& scene);

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

    /// Rewrite a rectangle of a terrain body's heights, in normalized [0,1]
    /// samples matching the .fjterrain array. `heights` is row-major and
    /// `size_x * size_z` long. Jolt updates its acceleration structure for the
    /// affected blocks only, which is what keeps a sculpt stroke cheap on the
    /// physics side — the render side does the matching partial texture upload.
    ///
    /// The rect is snapped outward to Jolt's block size, so the region written
    /// may be larger than the one requested. Heights outside [0,1] clamp.
    void update_terrain_heights(uint32_t object_id, uint32_t x, uint32_t z,
                                uint32_t size_x, uint32_t size_z,
                                const float* heights);

    /// Refresh which surface a terrain body reports under each quad after the
    /// layer weights changed in a rectangle of weight texels. The dominant
    /// painted layer's surface wins per quad, which is what the surface
    /// under a foot or a ray should be; the shader's slope and height rules
    /// are not consulted, so a cliff painted grass reports grass.
    void update_terrain_surfaces(uint32_t object_id, const class TerrainAsset& terrain,
                                 uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z);

    /// World-space ground height under a point, from the collision shape rather
    /// than a separate interpolation of the height array, so gameplay queries
    /// and what a character stands on cannot disagree. Returns false when
    /// nothing is below the point within `max_distance`.
    [[nodiscard]] bool ground_height_at(const glm::vec3& world_position,
                                        float max_distance, float& out_height) const;

    /// Ground normal under a point, from the same downward query as
    /// ground_height_at.
    [[nodiscard]] bool ground_normal_at(const glm::vec3& world_position,
                                        float max_distance, glm::vec3& out_normal) const;

    void set_gravity(const glm::vec3& gravity);
    [[nodiscard]] glm::vec3 gravity() const;

    /// Collision/trigger events collected during the last step.
    /// Each pair appears once. Cleared at the start of each step.
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& collision_enter_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& collision_exit_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& trigger_enter_events() const;
    [[nodiscard]] const std::vector<std::pair<uint32_t, CollisionEvent>>& trigger_exit_events() const;

private:
    void create_bone_bodies(Node& node, const class Skeleton& skeleton);
    void apply_character_changes(struct CharacterEntry& entry, CharacterComponent& character,
                                 const Node3D& node);

public:
    // Named here so the file-local helpers in physics_world.cpp can take it by
    // reference; it is defined there and nowhere else.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell

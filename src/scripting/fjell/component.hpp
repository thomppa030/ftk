// component.hpp — ScriptComponent base class for Fjell game code
#pragma once

#include "fjell/world.hpp"
#include "fjell/transform.hpp"

#include <cstdint>

namespace fjell {

struct Node3D;  // SDK handle — defined in fjell/node.hpp
class ScriptManager;

class ScriptComponent {
public:
    virtual ~ScriptComponent() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    virtual void on_collision_enter(uint32_t other_id, const glm::vec3& point, const glm::vec3& normal) {
        (void)other_id; (void)point; (void)normal;
    }
    virtual void on_collision_exit(uint32_t other_id) { (void)other_id; }
    virtual void on_trigger_enter(uint32_t other_id) { (void)other_id; }
    virtual void on_trigger_exit(uint32_t other_id) { (void)other_id; }

    [[nodiscard]] uint32_t id() const { return id_; }
    [[nodiscard]] const char* name() const { return name_; }
    [[nodiscard]] Transform& transform() { return *transform_; }
    [[nodiscard]] const Transform& transform() const { return *transform_; }
    inline Node3D self() const;  // defined in fjell/node.hpp

    // --- World state ---

    [[nodiscard]] const Input& input() const { return world_->input(); }
    [[nodiscard]] PhysicsWorld* physics() { return world_->physics(); }
    [[nodiscard]] float delta_time() const { return world_->delta_time; }
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

private:
    friend class ScriptManager;
    Node3D* node_{nullptr};
    World* world_{nullptr};
    Transform* transform_{nullptr};
    uint32_t id_{0};
    const char* name_{""};
};

using Component = ScriptComponent;

} // namespace fjell

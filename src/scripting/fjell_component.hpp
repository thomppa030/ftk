// fjell_component.hpp — ScriptComponent base class for Fjell game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_transform.hpp"
#include "fjell_world.hpp"

#include <cstdint>

namespace fjell {

class ScriptManager;
struct Node3D;

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
    [[nodiscard]] Node3D* node() { return node_; }
    [[nodiscard]] const Node3D* node() const { return node_; }

    // --- Subsystem access (thin wrappers over World) ---

    [[nodiscard]] const Input& input() const { return world_->input(); }
    [[nodiscard]] PhysicsWorld* physics() { return world_->physics(); }
    [[nodiscard]] float delta_time() const { return world_->delta_time; }
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }

    void destroy(uint32_t target_id) { world_->destroy(target_id); }

    void log(const char* msg) const { world_->log_info(msg); }
    void log_warn(const char* msg) const { world_->log_warn(msg); }
    void log_error(const char* msg) const { world_->log_error(msg); }

    template <typename... Args>
    void log_f(const char* fmt, Args&&... args) const {
        world_->log_info_f(fmt, args...);
    }

    // Kept for backward compat — prefer the direct accessors above
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

// Short alias for game scripts
using Component = ScriptComponent;

} // namespace fjell

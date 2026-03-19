#pragma once

#include "renderer/fly_camera.hpp"
#include "scene/scene.hpp"

#include <memory>
#include <vector>

namespace fjell {

class Input;
class PhysicsWorld;

class PlaySession {
public:
    PlaySession(Scene& scene, glm::vec3 camera_pos, float yaw, float pitch);

    void update(const Input& input, float dt);
    void physics_step(float dt, Scene& scene);

    [[nodiscard]] FlyCamera& camera() { return camera_; }
    [[nodiscard]] const FlyCamera& camera() const { return camera_; }
    [[nodiscard]] bool wants_stop(const Input& input) const;
    [[nodiscard]] PhysicsWorld* physics_world() { return physics_world_.get(); }

    void restore(Scene& scene);

private:
    FlyCamera camera_;
    std::vector<SceneNode> scene_snapshot_;
    uint32_t next_id_snapshot_;
    std::unique_ptr<PhysicsWorld> physics_world_;
};

} // namespace fjell

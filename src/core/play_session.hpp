#pragma once

#include "scene/scene.hpp"

#include <memory>
#include <vector>

namespace fjell {

class Input;
class PhysicsWorld;

class PlaySession {
public:
    explicit PlaySession(Scene& scene);

    void physics_step(float dt, Scene& scene);

    [[nodiscard]] bool wants_stop(const Input& input) const;
    [[nodiscard]] PhysicsWorld* physics_world() { return physics_world_.get(); }

    void restore(Scene& scene);

private:
    std::vector<std::unique_ptr<SceneNode>> scene_snapshot_;
    uint32_t next_id_snapshot_;
    std::unique_ptr<PhysicsWorld> physics_world_;
};

} // namespace fjell

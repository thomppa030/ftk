#pragma once

#include "renderer/fly_camera.hpp"
#include "scene/scene.hpp"

#include <vector>

namespace fjell {

class Input;

class PlaySession {
public:
    PlaySession(Scene& scene, glm::vec3 camera_pos, float yaw, float pitch);

    void update(const Input& input, float dt);

    [[nodiscard]] FlyCamera& camera() { return camera_; }
    [[nodiscard]] const FlyCamera& camera() const { return camera_; }
    [[nodiscard]] bool wants_stop(const Input& input) const;

    void restore(Scene& scene);

private:
    FlyCamera camera_;
    std::vector<RenderObject> scene_snapshot_;
    uint32_t next_id_snapshot_;
};

} // namespace fjell

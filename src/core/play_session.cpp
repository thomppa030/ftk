#include "core/play_session.hpp"
#include "core/input.hpp"

#include <GLFW/glfw3.h>

namespace fjell {

PlaySession::PlaySession(Scene& scene, glm::vec3 camera_pos, float yaw, float pitch)
    : camera_{camera_pos, yaw, pitch}
    , scene_snapshot_{scene.objects()}
    , next_id_snapshot_{scene.next_id()} {}

void PlaySession::update(const Input& input, float dt) {
    camera_.update(input, dt);
}

bool PlaySession::wants_stop(const Input& input) const {
    return input.is_key_pressed(GLFW_KEY_ESCAPE);
}

void PlaySession::restore(Scene& scene) {
    scene.objects() = scene_snapshot_;
    scene.set_next_id(next_id_snapshot_);
}

} // namespace fjell

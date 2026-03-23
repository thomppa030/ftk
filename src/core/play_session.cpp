#include "core/play_session.hpp"
#include "core/input.hpp"

#include <GLFW/glfw3.h>

namespace fjell {

PlaySession::PlaySession(Scene& scene)
    : next_id_snapshot_{scene.next_id()}
{
    scene_snapshot_.reserve(scene.objects().size());
    for (const auto& obj : scene.objects()) {
        scene_snapshot_.push_back(obj->clone());
    }
}

bool PlaySession::wants_stop(const Input& input) const {
    return input.is_key_pressed(GLFW_KEY_ESCAPE);
}

void PlaySession::restore(Scene& scene) {
    scene.objects().clear();
    scene.objects() = std::move(scene_snapshot_);
    scene.set_next_id(next_id_snapshot_);
}

} // namespace fjell

#include "audio/audio_subsystem.hpp"

#include "scene/camera_node.hpp"

namespace fjell {

void AudioSubsystem::tick(const TickContext& ctx) {
    if (!ctx.physics) return;

    for (const auto& obj : ctx.scene.objects()) {
        if (const auto* cam = dynamic_cast<const CameraNode*>(obj.get())) {
            glm::vec3 pos = cam->position();
            glm::vec3 fwd = -glm::normalize(glm::vec3(cam->world_matrix_[2]));
            glm::vec3 up = glm::normalize(glm::vec3(cam->world_matrix_[1]));
            engine_.update(ctx.scene, pos, fwd, up);
            return;
        }
    }
}

} // namespace fjell

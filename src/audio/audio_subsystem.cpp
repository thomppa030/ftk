#include "audio/audio_subsystem.hpp"

#include "audio/audio_asset.hpp"
#include "core/log.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/camera_node.hpp"

namespace fjell {

void AudioSubsystem::tick(const TickContext& ctx) {
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

void AudioSubsystem::play_oneshot(AudioAssetHandle asset_handle,
                                   const glm::vec3& position,
                                   ResourceManager& resources,
                                   float volume_scale, float pitch_scale,
                                   const Project* project) {
    auto* asset = resources.get(asset_handle);
    if (!asset) {
        FJELL_CORE_ERROR("Audio: invalid asset handle");
        return;
    }

    engine_.play_oneshot(asset->clip_path, position,
                         asset->volume * volume_scale,
                         asset->pitch * pitch_scale,
                         project);
}

} // namespace fjell

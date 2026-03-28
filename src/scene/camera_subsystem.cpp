#include "scene/camera_subsystem.hpp"
#include "core/frame_context.hpp"
#include "scene/camera_node.hpp"
#include "scene/scene.hpp"

namespace fjell {

void CameraSubsystem::on_play_start(const TickContext& ctx) {
    active_id_ = 0;

    // Find the first CameraNode in the scene
    for (const auto& obj : ctx.scene.objects()) {
        if (dynamic_cast<const CameraNode*>(obj.get())) {
            active_id_ = obj->id;
            break;
        }
    }
}

void CameraSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    active_id_ = 0;
}

void CameraSubsystem::set_active(uint32_t node_id) {
    active_id_ = node_id;
}

CameraNode* CameraSubsystem::active_camera(Scene& scene) {
    if (active_id_ == 0) return nullptr;
    auto* node = scene.find(active_id_);
    return node ? dynamic_cast<CameraNode*>(node) : nullptr;
}

const CameraNode* CameraSubsystem::active_camera(const Scene& scene) const {
    if (active_id_ == 0) return nullptr;
    const auto* node = scene.find(active_id_);
    return node ? dynamic_cast<const CameraNode*>(node) : nullptr;
}

} // namespace fjell

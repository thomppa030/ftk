#include "renderer/vfx_subsystem.hpp"

#include "renderer/particle_emitter.hpp"
#include "scene/scene.hpp"

namespace fjell {

void VfxSubsystem::tick(const TickContext& ctx) {
    for (auto it = oneshots_.begin(); it != oneshots_.end();) {
        it->remaining -= ctx.dt;
        if (it->remaining <= 0.0f) {
            ctx.scene.queue_destroy(it->node_id);
            it = oneshots_.erase(it);
        } else {
            ++it;
        }
    }
}

void VfxSubsystem::on_play_stop(const TickContext& ctx) {
    for (const auto& entry : oneshots_) {
        ctx.scene.queue_destroy(entry.node_id);
    }
    oneshots_.clear();
    ctx.scene.flush_destroyed();
}

void VfxSubsystem::spawn_particles_at(const std::string& /*vfx_path*/,
                                       const glm::vec3& position,
                                       Scene& scene) {
    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_oneshot";
    node->transform.position = position;

    auto& emitter = node->components.add<EmitterComponent>();
    emitter.emission_rate = 200.0f;
    emitter.lifetime_min = 0.3f;
    emitter.lifetime_max = 1.0f;
    emitter.max_particles = 500;

    float max_life = emitter.lifetime_max;
    uint32_t id = scene.add_node(std::move(node));
    oneshots_.push_back({id, max_life + 0.5f});
}

} // namespace fjell

#include "renderer/systems/vfx_subsystem.hpp"

#include "core/log.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
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

void VfxSubsystem::spawn(VfxAssetHandle asset_handle, const glm::vec3& position,
                          Scene& scene, ResourceManager& resources) {
    auto* effect = resources.get(asset_handle);
    if (!effect) {
        FJELL_CORE_ERROR("VFX: invalid asset handle");
        return;
    }

    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_oneshot";
    node->transform.position = position;

    auto& emitter = node->components.add<EmitterComponent>();
    // Load a fresh copy of the effect (modules contain unique_ptrs, can't copy)
    auto path = resources.asset_path_of(asset_handle);
    if (!path.empty()) {
        auto loaded = load_vfx_effect(path);

        // Resolve texture modules to bindless indices
        for (auto& em : loaded.emitters) {
            for (auto& mod : em.spawn_modules) {
                if (mod->type() == ModuleType::initial_texture) {
                    auto* tex = static_cast<InitialTextureModule*>(mod.get());
                    if (!tex->texture_path.empty()) {
                        auto r = resources.load_texture(tex->texture_path);
                        if (r) tex->bindless_index = resources.bindless_index(*r);
                    }
                }
            }
        }

        emitter.effect = std::make_shared<VfxEffect>(std::move(loaded));
        emitter.vfx_path = path;
    }

    float max_life = effect->max_lifetime();
    uint32_t id = scene.add_node(std::move(node));
    oneshots_.push_back({id, max_life + 0.5f});
}

} // namespace fjell

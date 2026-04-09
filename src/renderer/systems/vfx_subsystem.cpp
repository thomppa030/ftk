#include "renderer/systems/vfx_subsystem.hpp"

#include "core/log.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "scene/scene.hpp"

#include <fstream>

namespace fjell {

void VfxSubsystem::tick(const TickContext& ctx) {
    for (auto it = oneshots_.begin(); it != oneshots_.end();) {
        it->remaining -= ctx.dt;
        if (it->remaining <= 0.0f) {
            if (pool_.size() < MAX_POOL_SIZE) {
                auto* node = ctx.scene.find(it->node_id);
                if (node) {
                    node->components.remove<EmitterComponent>();
                    pool_.push_back({it->node_id, std::move(it->vfx_path)});
                }
            } else {
                ctx.scene.queue_destroy(it->node_id);
            }
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

    for (const auto& pooled : pool_) {
        ctx.scene.queue_destroy(pooled.node_id);
    }
    pool_.clear();

    json_cache_.clear();
    ctx.scene.flush_destroyed();
}

VfxEffect VfxSubsystem::build_effect(const std::string& path,
                                      ResourceManager& resources) {
    auto it = json_cache_.find(path);
    if (it == json_cache_.end()) {
        std::ifstream f(path);
        if (!f.is_open()) {
            FJELL_CORE_ERROR("VFX: failed to open {}", path);
            return {};
        }
        nlohmann::json j;
        f >> j;
        it = json_cache_.emplace(path, std::move(j)).first;
    }

    auto effect = load_vfx_effect_from_json(it->second);
    resolve_vfx_textures(effect, resources);
    return effect;
}

uint32_t VfxSubsystem::reclaim_pooled(const std::string& path,
                                       const glm::vec3& position,
                                       Scene& scene) {
    for (auto it = pool_.begin(); it != pool_.end(); ++it) {
        if (it->vfx_path == path) {
            uint32_t id = it->node_id;
            pool_.erase(it);

            auto* node = scene.find(id);
            if (!node) return 0;

            auto* spatial = node->as_spatial();
            if (spatial) spatial->transform.position = position;
            return id;
        }
    }
    return 0;
}

void VfxSubsystem::spawn(VfxAssetHandle asset_handle, const glm::vec3& position,
                          Scene& scene, ResourceManager& resources) {
    auto* raw_effect = resources.get(asset_handle);
    if (!raw_effect) {
        FJELL_CORE_ERROR("VFX: invalid asset handle");
        return;
    }

    auto path = resources.asset_path_of(asset_handle);
    if (path.empty()) return;

    auto effect = build_effect(path, resources);
    float max_life = effect.max_lifetime();

    // Try to reclaim a pooled node with the same effect
    uint32_t node_id = reclaim_pooled(path, position, scene);
    if (node_id != 0) {
        auto* node = scene.find(node_id);
        if (node) {
            auto& emitter = node->components.add<EmitterComponent>();
            emitter.effect = std::make_shared<VfxEffect>(std::move(effect));
            emitter.vfx_path = path;
            oneshots_.push_back({node_id, max_life + 0.5f, path});
            return;
        }
    }

    // No pooled node — create new
    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_oneshot";
    node->transform.position = position;

    auto& emitter = node->components.add<EmitterComponent>();
    emitter.effect = std::make_shared<VfxEffect>(std::move(effect));
    emitter.vfx_path = path;

    node_id = scene.add_node(std::move(node));
    oneshots_.push_back({node_id, max_life + 0.5f, path});
}

} // namespace fjell

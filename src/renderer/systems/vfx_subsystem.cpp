#include "renderer/systems/vfx_subsystem.hpp"

#include "ftk/base/log.hpp"
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
                                      ResourceManager& resources,
                                      const Project* project) {
    std::error_code ec;
    auto written = std::filesystem::last_write_time(path, ec);

    auto it = json_cache_.find(path);
    if (it == json_cache_.end() || it->second.written != written) {
        std::ifstream f(path);
        if (!f.is_open()) {
            FJELL_CORE_ERROR("VFX: failed to open {}", path);
            return {};
        }
        nlohmann::json j;
        f >> j;
        it = json_cache_.insert_or_assign(path, CachedJson{std::move(j), written}).first;
    }

    auto effect = load_vfx_effect_from_json(it->second.json);
    resolve_vfx_textures(effect, resources, project);
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
                          Scene& scene, ResourceManager& resources, const Project* project) {
    auto* raw_effect = resources.get(asset_handle);
    if (!raw_effect) {
        FJELL_CORE_ERROR("VFX: invalid asset handle");
        return;
    }

    auto path = resources.asset_path_of(asset_handle);
    if (path.empty()) return;

    auto effect = build_effect(path, resources, project);
    // A little past the end, so the last particles are gone before the
    // node is taken back rather than cut off mid-life.
    const float remaining = effect.time_to_finish() + 0.5f;

    // A one-shot plays its emitters through once; looping is for placed
    // emitters, and would keep a rate emitter going until the cut.
    auto attach = [&](Node& node) {
        auto& emitter = node.components.add<EmitterComponent>();
        emitter.effect = std::make_shared<VfxEffect>(std::move(effect));
        emitter.vfx_path = path;
        emitter.looping = false;
    };

    // Try to reclaim a pooled node with the same effect
    uint32_t node_id = reclaim_pooled(path, position, scene);
    if (node_id != 0) {
        auto* node = scene.find(node_id);
        if (node) {
            attach(*node);
            oneshots_.push_back({node_id, remaining, path});
            return;
        }
    }

    // No pooled node — create new
    auto node = std::make_unique<SceneNode>();
    node->name = "vfx_oneshot";
    node->transform.position = position;
    attach(*node);

    node_id = scene.add_node(std::move(node));
    oneshots_.push_back({node_id, remaining, path});
}

} // namespace fjell

#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/systems/vfx_emitter.hpp"

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class ResourceManager;

class VfxSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "VFX"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void tick(const TickContext& ctx) override;

    void on_play_stop(const TickContext& ctx) override;

    void spawn(VfxAssetHandle asset, const glm::vec3& position,
               Scene& scene, ResourceManager& resources);

private:
    struct OneshotEntry {
        uint32_t node_id;
        float remaining;
        std::string vfx_path; // for recycling into pool by effect
    };

    struct PooledNode {
        uint32_t node_id;
        std::string vfx_path;
    };

    /// Build a fresh VfxEffect from cached JSON (file I/O only on first load).
    VfxEffect build_effect(const std::string& path, ResourceManager& resources);

    /// Try to reclaim an idle pooled node for the same effect.
    uint32_t reclaim_pooled(const std::string& path, const glm::vec3& position,
                            Scene& scene);

    std::vector<OneshotEntry> oneshots_;
    std::vector<PooledNode> pool_;
    std::unordered_map<std::string, nlohmann::json> json_cache_;

    static constexpr uint32_t MAX_POOL_SIZE = 32;
};

} // namespace fjell

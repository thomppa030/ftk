#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "renderer/systems/vfx_emitter.hpp"

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class Project;
class ResourceManager;

class VfxSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "VFX"; }

    void tick(const TickContext& ctx) override;

    void on_play_stop(const TickContext& ctx) override;

    /// Play the effect once at `position` and take the node back when it is
    /// over. The project is what its texture and material references are
    /// resolved against.
    void spawn(VfxAssetHandle asset, const glm::vec3& position,
               Scene& scene, ResourceManager& resources, const Project* project);

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

    /// Build a fresh VfxEffect from cached JSON; the file is read again
    /// only when it has changed on disk.
    VfxEffect build_effect(const std::string& path, ResourceManager& resources,
                           const Project* project);

    struct CachedJson {
        nlohmann::json json;
        std::filesystem::file_time_type written;
    };

    /// Try to reclaim an idle pooled node for the same effect.
    uint32_t reclaim_pooled(const std::string& path, const glm::vec3& position,
                            Scene& scene);

    std::vector<OneshotEntry> oneshots_;
    std::vector<PooledNode> pool_;
    std::unordered_map<std::string, CachedJson> json_cache_;

    static constexpr uint32_t MAX_POOL_SIZE = 32;
};

} // namespace fjell

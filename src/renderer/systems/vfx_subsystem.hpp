#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
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
    };
    std::vector<OneshotEntry> oneshots_;
};

} // namespace fjell

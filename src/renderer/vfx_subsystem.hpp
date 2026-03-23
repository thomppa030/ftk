#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

class VfxSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "VFX"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void tick(const TickContext& ctx) override;

    void on_play_stop(const TickContext& ctx) override;

    void spawn_particles_at(const std::string& vfx_path, const glm::vec3& position,
                            Scene& scene);

private:
    struct OneshotEntry {
        uint32_t node_id;
        float remaining;
    };
    std::vector<OneshotEntry> oneshots_;
};

} // namespace fjell

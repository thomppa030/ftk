#pragma once

#include "renderer/particle_emitter.hpp"

#include <string>

namespace fjell {

struct VfxAsset {
    float emission_rate{50.0f};
    float lifetime_min{1.0f};
    float lifetime_max{3.0f};
    uint32_t max_particles{10000};
    glm::vec3 velocity_min{-0.5f, 1.0f, -0.5f};
    glm::vec3 velocity_max{0.5f, 3.0f, 0.5f};
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    glm::vec4 color_start{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4 color_end{1.0f, 0.1f, 0.0f, 0.0f};
    float size_start{0.1f};
    float size_end{0.02f};
    ParticleBlendMode blend_mode{ParticleBlendMode::additive};
};

[[nodiscard]] VfxAsset load_vfx_asset_file(const std::string& path);
void save_vfx_asset_file(const std::string& path, const VfxAsset& asset);
void apply_vfx_to_emitter(const VfxAsset& asset, EmitterComponent& emitter);

} // namespace fjell

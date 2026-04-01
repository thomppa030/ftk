#pragma once

#include "renderer/components/particle_emitter.hpp"

#include <string>

namespace fjell {

struct VfxEffect;

// Legacy v1 flat asset (backward compat)
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

/// Load a .fjvfx file — auto-detects v1 (flat) or v2 (module stacks).
[[nodiscard]] VfxEffect load_vfx_effect(const std::string& path);

/// Save a VfxEffect as .fjvfx v2.
void save_vfx_effect(const std::string& path, const VfxEffect& effect);

} // namespace fjell

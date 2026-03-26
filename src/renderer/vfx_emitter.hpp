#pragma once

#include "renderer/particle_emitter.hpp"
#include "renderer/vfx_module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

/// An emitter defined by ordered module stacks (Niagara-lite).
struct VfxEmitter {
    std::string name{"emitter"};
    uint32_t max_particles{10000};
    ParticleBlendMode blend_mode{ParticleBlendMode::additive};

    std::vector<std::unique_ptr<VfxModule>> spawn_modules;
    std::vector<std::unique_ptr<VfxModule>> update_modules;
    std::vector<std::unique_ptr<VfxModule>> render_modules;

    /// Build a default emitter with the standard module stack
    static VfxEmitter make_default();

    /// Build from a legacy flat EmitterComponent (v1 compat)
    static VfxEmitter from_legacy(const EmitterComponent& emitter);

    /// Extract emission config from spawn modules
    [[nodiscard]] float emission_rate() const;
    [[nodiscard]] float lifetime_min() const;
    [[nodiscard]] float lifetime_max() const;

    /// Serialize all spawn + update modules to a flat GPU buffer.
    uint32_t write_gpu_modules(uint8_t* buffer) const;

    /// Compute required buffer size for all modules
    [[nodiscard]] uint32_t gpu_data_size() const;

    // Runtime (not serialized, managed by ParticleSystem)
    float emit_accumulator{0.0f};
};

/// A complete VFX effect — one or more emitters that play together.
/// This is the asset-level container. One scene node → one VfxEffect.
struct VfxEffect {
    std::string name{"effect"};
    std::vector<VfxEmitter> emitters;

    /// Build from a legacy single EmitterComponent
    static VfxEffect from_legacy(const EmitterComponent& ec);

    /// Total max particles across all emitters
    [[nodiscard]] uint32_t total_max_particles() const;

    /// Max lifetime across all emitters (for oneshot duration)
    [[nodiscard]] float max_lifetime() const;
};

} // namespace fjell

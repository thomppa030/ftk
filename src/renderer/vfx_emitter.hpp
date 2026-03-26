#pragma once

#include "renderer/particle_emitter.hpp"
#include "renderer/vfx_module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

/// An emitter defined by ordered module stacks (Niagara-lite).
/// Replaces the flat EmitterComponent config with a modular system.
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

    /// Apply this emitter's spawn modules to extract emission config
    /// (rate, lifetime range) for CPU-side accumulation
    [[nodiscard]] float emission_rate() const;
    [[nodiscard]] float lifetime_min() const;
    [[nodiscard]] float lifetime_max() const;

    /// Serialize all spawn + update modules to a flat GPU buffer.
    /// Returns total bytes written. Buffer must be large enough (use gpu_data_size()).
    uint32_t write_gpu_modules(uint8_t* buffer) const;

    /// Compute required buffer size for all modules
    [[nodiscard]] uint32_t gpu_data_size() const;
};

} // namespace fjell

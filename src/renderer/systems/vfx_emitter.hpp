#pragma once

#include "renderer/components/particle_emitter.hpp"
#include "renderer/systems/vfx_expression.hpp"
#include "renderer/systems/vfx_module.hpp"

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

    ExpressionGraph expression_graph;

    // ── Timing (serialized) ───────────────────────────────────────────
    float start_delay{0.0f};   // seconds before this emitter begins
    float duration{0.0f};      // emitter active time (0 = infinite / use particle lifetime)
    uint32_t loop_count{0};    // 0 = infinite, N = play N times then stop

    /// Effective duration: explicit duration, or particle lifetime if 0
    [[nodiscard]] float effective_duration() const;

    /// Build a default emitter with the standard module stack
    static VfxEmitter make_default();

    /// Extract emission config from spawn modules
    [[nodiscard]] float emission_rate() const;
    [[nodiscard]] uint32_t burst_count() const;
    [[nodiscard]] float lifetime_min() const;
    [[nodiscard]] float lifetime_max() const;

    /// Serialize all spawn + update modules to a flat GPU buffer.
    uint32_t write_gpu_modules(uint8_t* buffer) const;

    /// Compute required buffer size for all modules
    [[nodiscard]] uint32_t gpu_data_size() const;

    // ── Runtime (not serialized, managed by ParticleSystem) ──────────
    float emit_accumulator{0.0f};
    float distance_accumulator{0.0f};
    float burst_elapsed{0.0f};
    float emitter_elapsed{0.0f};   // time since emitter started (after delay)
    uint32_t loops_completed{0};   // how many full cycles have finished
    bool burst_fired{false};
    bool paused{false};            // suppress emission + freeze simulation
    bool finished{false};          // emitter completed all loops
};

/// A complete VFX effect — one or more emitters that play together.
/// This is the asset-level container. One scene node → one VfxEffect.
struct VfxEffect {
    std::string name{"effect"};
    std::vector<VfxEmitter> emitters;

    /// Total max particles across all emitters
    [[nodiscard]] uint32_t total_max_particles() const;

    /// Max lifetime across all emitters (for oneshot duration)
    [[nodiscard]] float max_lifetime() const;

    /// Total effect duration (max of start_delay + effective_duration across emitters)
    [[nodiscard]] float total_duration() const;

    /// Reset all emitter runtime state (elapsed, burst, accumulators)
    void reset_playback();
};

} // namespace fjell

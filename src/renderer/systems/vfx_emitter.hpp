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

    // When true, the expression graph's `time` node reads local_elapsed
    // (zeroed on every burst fire and loop restart) instead of the engine's
    // shared, never-reset clock — each firing gets its own instance of
    // time, starting fresh, rather than sampling whatever phase the
    // engine's clock happens to be at. Off by default so existing effects
    // keep their current look — turn it on for anything whose expression
    // graph should replay identically every time it fires, e.g. a burst
    // whose swirl phase shouldn't depend on how long the game's been
    // running.
    bool instanced_time{false};

    /// Effective duration: explicit duration, or particle lifetime if 0
    [[nodiscard]] float effective_duration() const;

    /// Build a default emitter with the standard module stack
    static VfxEmitter make_default();

    /// Extract emission config from spawn modules
    [[nodiscard]] float emission_rate() const;
    [[nodiscard]] uint32_t burst_count() const;
    [[nodiscard]] float lifetime_min() const;
    [[nodiscard]] float lifetime_max() const;

    /// Serialize all spawn + update modules to a flat GPU buffer, capped at
    /// max_bytes. If the modules don't fit, later modules are dropped (with
    /// a terminator still written within budget) rather than overrunning
    /// into whatever follows in the caller's buffer — see the .cpp for why
    /// this can't just be a size check ahead of time.
    uint32_t write_gpu_modules(uint8_t* buffer, uint32_t max_bytes) const;

    // ── Runtime (not serialized, managed by ParticleSystem) ──────────
    float emit_accumulator{0.0f};
    float distance_accumulator{0.0f};
    float burst_elapsed{0.0f};
    float emitter_elapsed{0.0f};   // time since emitter started (after delay)
    float local_elapsed{0.0f};     // time since last burst fire / loop restart;
                                    // only meaningful when instanced_time is set
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

    /// When a one-shot of this effect is over: its last emitter has stopped
    /// emitting and the last particle that emitter could have made has died.
    [[nodiscard]] float time_to_finish() const;

    /// Reset all emitter runtime state (elapsed, burst, accumulators)
    void reset_playback();
};

} // namespace fjell

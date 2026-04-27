#pragma once

#include "renderer/resource_registry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class OffscreenPass;
class PassRegistry;
class PostProcessResources;
class RenderPass;
class ThreadPool;
class ThreadCommandPools;
struct FrameContext;

/// Execution phases for render passes. Each phase has a base order value;
/// passes within a phase are ordered by sub_order (0-99).
enum class RenderPhase : uint32_t {
    Environment  = 0,     // sky cubemap, shadows
    DepthPrepass = 200,   // depth rendering, hi-z build
    GpuDriven    = 400,   // meshlet cull, GTAO, light cull, subsystem compute
    Color        = 600,   // opaque, DDGI, screen color copy, distortion, transparent
    PostProcess  = 700,   // SSR, volumetric fog, bloom, tonemap
    Debug        = 1100,  // debug visualization passes
};

/// A self-declaring pass registration: name, execution phase + sub-order,
/// and a pointer to the RenderPass for declare()/record() dispatch.
struct PassEntry {
    std::string name;
    uint32_t order{0};

    /// Non-owning pointer to the RenderPass the registry holds. The
    /// pipeline invokes declare()/record() through this pointer.
    RenderPass* pass{nullptr};

    /// Construct order from phase + sub_order within that phase.
    static constexpr uint32_t make_order(RenderPhase phase, uint32_t sub_order = 0) {
        return static_cast<uint32_t>(phase) + sub_order;
    }
};

/// Snapshot of the most recent frame's aliasing stats. Populated by
/// execute(); read by the stats panel. Safe to read on the main thread
/// between frames.
struct AliasingStats {
    uint32_t logical_count{0};   // number of virtual (create()) resources in use
    uint32_t physical_count{0};  // number of AliasGroups returned
    uint32_t live_images{0};     // TransientImagePool cached images
    uint64_t live_bytes{0};      // TransientImagePool total VRAM in bytes
    uint32_t acquires{0};        // pool acquires this frame
    uint32_t allocations{0};     // pool allocations (misses) this frame
};

/// Pass registry + frame graph execution. Passes register themselves with
/// their image dependencies; execute() builds and runs the frame graph.
class RenderPipeline {
public:
    ~RenderPipeline();

    void register_pass(PassEntry entry);
    void remove_pass(const std::string& name);

    /// Enable or disable a pass by name. Disabled passes are skipped during execute().
    void set_pass_enabled(const std::string& name, bool enabled);

    /// Query whether a pass is enabled.
    [[nodiscard]] bool is_pass_enabled(const std::string& name) const;

    /// Debug toggle: when false, FrameGraph::compute_alias_groups() emits
    /// one group per logical resource (no packing), so the pool allocates
    /// a dedicated VkImage per logical. Intended for isolating regressions
    /// that may stem from aliasing reuse.
    void set_aliasing_enabled(bool enabled) noexcept { aliasing_enabled_ = enabled; }
    [[nodiscard]] bool aliasing_enabled() const noexcept { return aliasing_enabled_; }

    /// Most recent frame's aliasing snapshot. Updated every time execute()
    /// runs with a transient pool present.
    [[nodiscard]] const AliasingStats& aliasing_stats() const noexcept { return aliasing_stats_; }

    /// Peak aliasing values seen since startup. Useful for comparing runs
    /// without watching the live counter for the worst-case frame.
    [[nodiscard]] uint32_t peak_logical()     const noexcept { return peak_logical_; }
    [[nodiscard]] uint32_t peak_physical()    const noexcept { return peak_physical_; }
    [[nodiscard]] uint64_t peak_live_bytes()  const noexcept { return peak_live_bytes_; }
    [[nodiscard]] uint64_t peak_bytes_saved() const noexcept { return peak_bytes_saved_; }

    /// Build the frame graph from registered passes and execute it.
    /// `ctx` is captured by reference into per-pass record lambdas; the
    /// caller must keep it alive until this function returns.
    /// `async_compute_available` flips async-hinted passes onto the
    /// compute queue when true; false forces them back to graphics.
    /// Returns true if any pass recorded to the async compute CB — the
    /// caller uses this to decide whether to submit it and wait on its
    /// timeline semaphore.
    [[nodiscard]] bool execute(VkCommandBuffer cmd, OffscreenPass& target,
                               VkSampleCountFlagBits msaa, uint32_t active_sources,
                               ThreadPool& pool, ThreadCommandPools& secondary_pools,
                               uint32_t frame_index, ResourceRegistry& registry,
                               PassRegistry& passes, PostProcessResources* pp_resources,
                               const FrameContext& ctx,
                               bool async_compute_available);

private:
    struct PassState {
        bool enabled{true};
    };

    [[nodiscard]] PassState& state_for(const std::string& name);
    [[nodiscard]] const PassState& state_for(const std::string& name) const;

    std::vector<PassEntry> entries_;
    std::unordered_map<std::string, PassState> pass_state_;
    bool sorted_{false};

    /// Log the declared-DAG pass count and culled-pass count once per
    /// session instead of every frame.
    bool dag_order_logged_{false};

    /// Log resource lifetimes once per session (debug; gated by the
    /// FJELL_LOG_LIFETIMES env var inside FrameGraph::log_lifetimes).
    bool lifetimes_logged_{false};

    /// Master switch for C2 greedy packing. Flipped via the debug UI to
    /// diff aliased vs non-aliased runs.
    bool aliasing_enabled_{true};

    /// Last frame's aliasing snapshot, read by StatsPanel.
    AliasingStats aliasing_stats_{};

    /// Peak logical/physical counts + bytes seen across the session. Used
    /// by the shutdown summary log to report the achieved aliasing win.
    uint32_t peak_logical_{0};
    uint32_t peak_physical_{0};
    uint64_t peak_live_bytes_{0};
    uint64_t peak_bytes_saved_{0};

    static const PassState default_state_;
};

} // namespace fjell

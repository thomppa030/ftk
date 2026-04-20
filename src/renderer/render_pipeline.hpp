#pragma once

#include "renderer/resource_registry.hpp"

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

/// Pass registry + frame graph execution. Passes register themselves with
/// their image dependencies; execute() builds and runs the frame graph.
class RenderPipeline {
public:
    void register_pass(PassEntry entry);
    void remove_pass(const std::string& name);

    /// Enable or disable a pass by name. Disabled passes are skipped during execute().
    void set_pass_enabled(const std::string& name, bool enabled);

    /// Query whether a pass is enabled.
    [[nodiscard]] bool is_pass_enabled(const std::string& name) const;

    /// Build the frame graph from registered passes and execute it.
    /// `ctx` is captured by reference into per-pass record lambdas; the
    /// caller must keep it alive until this function returns.
    void execute(VkCommandBuffer cmd, OffscreenPass& target,
                 VkSampleCountFlagBits msaa, uint32_t active_sources,
                 ThreadPool& pool, ThreadCommandPools& secondary_pools,
                 uint32_t frame_index, ResourceRegistry& registry,
                 PassRegistry& passes, PostProcessResources* pp_resources,
                 const FrameContext& ctx);

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

    static const PassState default_state_;
};

} // namespace fjell

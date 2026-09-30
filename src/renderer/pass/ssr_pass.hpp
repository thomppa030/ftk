#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/render_settings.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

// ── Screen-space reflections pipeline ───────────────────────────────────────
//
// SSR runs two compute dispatches authored as two independent DAG nodes:
//
//   ssr_trace     — hiz + prev-frame color → ssr_output.
//   ssr_composite — ssr_output + MR → scene color.
//
// With each dispatch as its own RenderPass, the FrameGraph emits one
// merged barrier per image per node and ssr_output's lifetime collapses
// to [trace..composite]. The win is small (the monolithic pass already
// kept ssr_output live across only two pass-boundaries), but the split
// keeps us consistent with the rest of the post-process chain.
//
// Layering:
//   • SsrCore          — abstract base. Owns both samplers, both
//                        pipelines, the SSR parameters, and the
//                        cross-frame prev_frame history (sized in
//                        pre_record, filled by SsrHistoryPass).
//   • SsrTracePass     — concrete SsrCore. Registered as "ssr_trace".
//                        Records the trace dispatch or, when SSR is
//                        disabled, clears ssr_output to black so down-
//                        stream reads return zero.
//   • SsrCompositePass — follower. Holds SsrCore* set in post_create()
//                        via passes.get<SsrCore>("ssr_trace"). Records
//                        the composite dispatch.
//
// Resource flow:
//   • ssr_output is a graph transient — trace create()s it, composite
//     create()s it again for the dedup-by-name read.
//   • prev_frame_color is SsrCore's own image, exported to the graph.
//     SsrHistoryPass copies the scene colour into it once the last pass
//     that writes the colour has run; the next frame's trace reads it.
//
// Barrier contract: nodes declare only the accesses they perform, and the
// graph moves ssr_output between the trace's write (or the clear, when
// the trace is off) and the composite's read.

class SsrCore : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void collect_exports(DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;

    /// The history at `width` × `height`: made again, black, when its size
    /// changes. The frames still reading the old one keep it until they are
    /// done.
    void ensure_prev_frame(uint32_t width, uint32_t height);

    /// Copies the scene colour, in `Access::copy_src`, into the history, in
    /// `Access::copy_dst`. Skipped while the history does not have `extent`.
    void copy_prev_frame(gpu::CommandList& cmd, gpu::Texture color, glm::uvec2 extent);

    using Mode = RenderSettings::Ssr::Mode;
    void set_mode(Mode m) { mode_ = m; }
    void set_max_steps(uint32_t s) { max_steps_ = s; }
    void set_max_distance(float d) { max_distance_ = d; }
    void set_thickness(float t) { thickness_ = t; }
    void set_intensity(float i) { intensity_ = i; }

    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool screen_space_active() const { return mode_ == Mode::ScreenSpace; }
    [[nodiscard]] bool ray_traced_active() const { return mode_ == Mode::RayTraced; }
    [[nodiscard]] uint32_t max_steps() const { return max_steps_; }
    [[nodiscard]] float max_distance() const { return max_distance_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] float intensity() const { return intensity_; }

    // Getters for follower passes.
    [[nodiscard]] gpu::Sampler linear_sampler() const { return linear_sampler_; }
    [[nodiscard]] gpu::Sampler nearest_sampler() const { return nearest_sampler_; }
    [[nodiscard]] gpu::ComputePipeline trace_pipeline() const { return trace_pipeline_; }
    [[nodiscard]] gpu::ComputePipeline composite_pipeline() const { return composite_pipeline_; }
    [[nodiscard]] gpu::Texture prev_frame() const { return prev_frame_; }

private:
    gpu::Device* device_{nullptr};
    gpu::Sampler linear_sampler_;
    gpu::Sampler nearest_sampler_;
    gpu::Owned<gpu::ComputePipeline> trace_pipeline_;
    gpu::Owned<gpu::ComputePipeline> composite_pipeline_;

    Mode mode_{Mode::ScreenSpace};
    uint32_t max_steps_{64};
    float max_distance_{100.0f};
    float thickness_{0.3f};
    float intensity_{1.0f};

    /// Last frame's scene colour, which the trace takes what it hits from.
    gpu::Owned<gpu::Texture> prev_frame_;
    glm::uvec2 prev_frame_extent_{0, 0};
};

class SsrTracePass : public SsrCore {
public:
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
};

class SsrCompositePass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    SsrCore* core_{nullptr};
};

/// Copies the finished scene colour into the SSR history for the next
/// frame's trace. A node of its own, placed after the last pass that
/// writes the colour, so the graph orders the copy after that write and
/// moves the colour into the copy's layout and out again.
class SsrHistoryPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    SsrCore* core_{nullptr};
};

// Deprecated alias — stats_panel and other callers still reach in via
// `ssr_pass()` with type SsrPass. Point them at the abstract core.
using SsrPass = SsrCore;

} // namespace fjell

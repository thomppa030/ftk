#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

// ── Screen-space ambient occlusion pipeline ─────────────────────────────────
//
// AO runs three compute dispatches authored as three independent DAG nodes:
//
//   ao_trace    — trace occlusion, write half-res ao_image. Algorithm-specific
//                 (GTAO today; VBAO/HBAO can live alongside as sibling AoPass
//                 subclasses).
//   ao_blur_h   — horizontal separable blur, writes transient gtao_blur.
//   ao_blur_v   — vertical blur, writes back to ao_image.
//
// Each dispatch is its own RenderPass so the FrameGraph can schedule barriers
// per node (one merged barrier per image per node) and the blur scratch
// participates in alias bin-packing — its lifetime is only [blur_h, blur_v].
//
// Layering:
//   • AoPass        — abstract base. Owns shared state (the AO Images,
//                     ao_sampler, blur pipeline, shared parameters). Itself a
//                     RenderPass registered as "ao_trace". Subclasses
//                     implement the trace dispatch.
//   • GtaoTracePass — concrete AoPass. Owns the GTAO trace pipeline and
//                     implements the trace hooks.
//   • AoBlurHPass,
//     AoBlurVPass   — follower passes. Hold AoPass* set in post_create() via
//                     passes.get<AoPass>("ao_trace"). Record one blur dispatch
//                     each, reading the owner's shared state.
//
// Runtime algorithm switching: register exactly one AoPass subclass under
// "ao_trace". To support GTAO/VBAO toggles, register both and use
// RenderPipeline::set_pass_enabled to flip between them.
//
// Resource flow:
//   • the AO texture is owned by AoPass::Images, per viewport, and imported
//     by RenderPipeline::execute() as "gtao_ao", persistent (the global
//     set's "ao_texture" and the ImGui thumbnail read it across frames).
//   • gtao_blur is a graph transient declared by AoBlurHPass via create().
//     Lifetime [blur_h, blur_v], alias-eligible with other half-res R8
//     transients.
//
// Barrier contract: nodes declare reads/writes; the graph emits one merged
// barrier per image per node, and no node emits one of its own. The AO
// image rests where the global descriptor samples it, and the graph returns
// it there after each node that writes it.

class AoPass : public RenderPass {
public:
    struct Images {
        /// Half the viewport's size. Written on the compute queue when GTAO
        /// runs there and sampled by every fragment shader on the graphics
        /// queue, so it is shared by both.
        gpu::Owned<gpu::Texture> ao;
        glm::uvec2 extent{0, 0};

        /// No occlusion until the first trace writes it.
        void create(gpu::Device& gpu, uint32_t half_w, uint32_t half_h);
        /// Releases the texture; the frames still reading it keep it until
        /// they are done.
        void destroy();
        /// Made again at half of `width` × `height` when that changes.
        void resize(gpu::Device& gpu, uint32_t width, uint32_t height);
    };

    Result<> create(const PassResources& resources) final;
    void destroy() final;
    void record(const FrameContext& ctx) final;
    void declare(PassBuilder& builder, const DeclareContext& ctx) final;
    void collect_exports(DeclareContext& ctx) final;

    void resize(uint32_t width, uint32_t height);

    /// The placeholder AO for views without their own.
    [[nodiscard]] gpu::Texture ao_texture() const { return default_images_.ao; }
    [[nodiscard]] gpu::Sampler ao_sampler() const { return ao_sampler_; }

    void set_radius(float r) { radius_ = r; }
    void set_intensity(float i) { intensity_ = i; }
    void set_thickness(float t) { thickness_ = t; }
    void set_enabled(bool e) { enabled_ = e; }
    [[nodiscard]] float radius() const { return radius_; }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] gpu::ComputePipeline blur_pipeline() const { return blur_pipeline_; }
    [[nodiscard]] Images& images() { return default_images_; }

protected:
    // Subclass hooks. AoPass::create() sets up shared state then calls
    // create_trace(); AoPass::declare/record delegate when enabled, and
    // handle the !enabled_ clear-to-white path directly.
    virtual Result<> create_trace(const PassResources& resources) = 0;
    virtual void declare_trace(PassBuilder& builder, const DeclareContext& ctx) = 0;
    virtual void record_trace(const FrameContext& ctx, Images& images) = 0;
    virtual void destroy_trace() = 0;

    gpu::Device* device_{nullptr};

private:
    gpu::Sampler ao_sampler_;
    gpu::Owned<gpu::ComputePipeline> blur_pipeline_;

    float radius_{1.0F};
    float intensity_{1.5F};
    float thickness_{0.5F};
    bool enabled_{true};

    Images default_images_;
};

// GTAO — "Ground-Truth Ambient Occlusion", horizon-based screen-space AO.
class GtaoTracePass : public AoPass {
public:
    [[nodiscard]] PassSetup setup() const override;

protected:
    Result<> create_trace(const PassResources& resources) override;
    void declare_trace(PassBuilder& builder, const DeclareContext& ctx) override;
    void record_trace(const FrameContext& ctx, Images& images) override;
    void destroy_trace() override;

private:
    gpu::Owned<gpu::ComputePipeline> trace_pipeline_;
};

// Horizontal blur dispatch. Reads ao_image, writes transient gtao_blur.
class AoBlurHPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    AoPass* owner_{nullptr};
};

// Vertical blur dispatch. Reads transient gtao_blur, writes ao_image.
class AoBlurVPass : public RenderPass {
public:
    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

private:
    AoPass* owner_{nullptr};
};

} // namespace fjell

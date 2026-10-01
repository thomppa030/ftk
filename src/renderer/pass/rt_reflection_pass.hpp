#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/frustum.hpp"
#include "renderer/render_settings.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace fjell {

class AccelStructureManager;
class SkyCubemapPass;

/// Reprojection matrices for the reflection denoise.
///
/// Three mat4s do not fit the guaranteed 128-byte push constant range, so
/// they travel in a uniform block (the same reason `GpuSsGiConstants` exists).
struct GpuReflectionDenoiseConstants {
    glm::mat4 projection{1.0F};
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::mat4 prev_inv_view_proj{1.0F};
    glm::vec3 camera_position{0.0F};
    float pad0{0.0F};
};

// Ray-traced reflections. Slots into the same composite as SSR by writing
// rt_reflection_output (RGBA16F, viewport-sized), which the composite reads
// in place of ssr_output when the mode on SsrCore is ray traced.
//
// Skinned meshes are not in the BLAS in V1 — they don't appear in
// reflections. Static meshes and particles do.
//
// Hardware-gated: the pipelines are only built on a device with ray
// queries. Without them the pass still instantiates so .fjgraph parses,
// but declares and records nothing.
class RtReflectionPass : public RenderPass {
public:
    using Mode = RenderSettings::Ssr::Mode;

    /// The name each viewport's output is exported under.
    static constexpr std::string_view OUTPUT_NAME = "rt_reflection_output";

    /// Per-viewport textures, owned by PostProcessResources. All but the
    /// output are the pass's own and rest where its compute shaders sample
    /// them; they are made cleared, which leaves them there.
    struct Images {
        /// What the composite reads. The trace writes its one noisy sample
        /// per pixel here, the denoise reads it, and the spatial filter
        /// writes the finished reflection over it.
        gpu::Owned<gpu::Texture> output;
        /// The denoise's accumulation of the trace, before the filter.
        gpu::Owned<gpu::Texture> accumulated;
        /// Last frame's `accumulated`, which this frame's blends onto.
        gpu::Owned<gpu::Texture> history;
        /// Depth as the denoise saw it, for the disocclusion test. Two,
        /// ping-ponged: the dispatch samples last frame's at an arbitrary
        /// reprojected texel while writing this frame's, so one would race
        /// itself. R32F colour, written by the shader — see the note in
        /// rt_reflection_denoise.comp on why this is not a copy of the depth
        /// buffer.
        std::array<gpu::Owned<gpu::Texture>, 2> prev_depth;
        uint32_t prev_depth_slot{0};
        glm::uvec2 extent{0, 0};
        bool history_valid{false};

        // Per-viewport camera history. Each viewport reprojects through its own
        // previous camera — a shared copy would let the last viewport to render
        // decide what "previous frame" means for all the others (see
        // ViewHistory's own note).
        glm::mat4 prev_view_proj{1.0F};
        glm::mat4 prev_inv_view_proj{1.0F};

        void create(gpu::Device& gpu, uint32_t width, uint32_t height);
        /// Releases the textures; the frames still reading them keep them
        /// until they are done.
        void destroy();
        void resize(gpu::Device& gpu, uint32_t width, uint32_t height);
    };

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }
    void set_mode(Mode m) { mode_ = m; }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool ray_traced_active() const {
        return mode_ == Mode::RayTraced && trace_pipeline_;
    }

    void set_sun_light(glm::vec3 direction, glm::vec3 color, float intensity) {
        sun_direction_ = direction;
        sun_color_ = color;
        sun_intensity_ = intensity;
    }

    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] float intensity() const { return intensity_; }

    /// How far a reflection ray travels, in metres. The same value bounds the
    /// trace and is declared to the acceleration structure as this pass's
    /// reach, so the TLAS is built to cover what the rays can actually visit.
    /// Read them from here rather than restating the distance — a reach
    /// shorter than the trace is invisible until something reflective is far
    /// enough from the probe grid to notice, which is the bug this replaced.
    void set_max_trace_dist(float d) { max_trace_dist_ = d; }
    [[nodiscard]] float max_trace_dist() const { return max_trace_dist_; }

    /// The coarsest cell in the traced geometry, in metres — terrain's ray
    /// mesh spacing, which is the only geometry traced coarser than it is
    /// shaded. Rays start further out of the surface on slopes by this much,
    /// or they begin inside the mesh they are about to trace. Zero when no
    /// terrain is loaded, which puts rays back on the flat-ground minimum.
    void set_trace_cell_size(float m) { trace_cell_size_ = m; }
    [[nodiscard]] float trace_cell_size() const { return trace_cell_size_; }

    /// The box this pass's rays can reach from `camera`, for the scene's
    /// acceleration structure (VulkanContext::prepare_ray_tracing). Rays leave the camera in
    /// every direction, so it is a cube around it rather than a frustum:
    /// a reflection bounces backwards as readily as forwards.
    [[nodiscard]] AABB trace_reach_bounds(const glm::vec3& camera) const {
        return {camera - glm::vec3(max_trace_dist_), camera + glm::vec3(max_trace_dist_)};
    }

private:
    void record_into(const FrameContext& ctx, Images& imgs);
    void record_denoise(const FrameContext& ctx, Images& imgs);
    void record_spatial(const FrameContext& ctx, Images& imgs);

    AccelStructureManager* accel_manager_{nullptr};
    /// Where a ray that misses finds the sky.
    const SkyCubemapPass* sky_{nullptr};

    Mode mode_{Mode::ScreenSpace};
    float intensity_{1.0F};
    float max_trace_dist_{50.0F};
    float trace_cell_size_{0.0F};
    uint32_t frame_counter_{0};

    // Temporal blend: how much of the accumulated history survives each frame.
    // 0.9 converges in roughly ten frames. Tune against the rain scene rather
    // than a static one — the ripples animate the reflected image on their own,
    // and a value picked while nothing moves will visibly smear them.
    float hysteresis_{0.9F};

    glm::vec3 sun_direction_{0.0F, -1.0F, 0.0F};
    glm::vec3 sun_color_{1.0F};
    float sun_intensity_{1.5F};

    gpu::Sampler linear_sampler_;
    gpu::Sampler nearest_sampler_;
    /// Traces with ray queries; null on a device without them, as are the
    /// denoise and the filter.
    gpu::Owned<gpu::ComputePipeline> trace_pipeline_;
    /// Temporal accumulation of the trace.
    gpu::Owned<gpu::ComputePipeline> denoise_pipeline_;
    /// Spatial filter, run on the accumulated result.
    gpu::Owned<gpu::ComputePipeline> spatial_pipeline_;

    // Scales the roughness-driven kernel radius of the spatial filter; 0
    // leaves the accumulated image unfiltered.
    float spatial_strength_{1.0F};
};

} // namespace fjell

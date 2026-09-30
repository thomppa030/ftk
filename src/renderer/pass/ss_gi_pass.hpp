#pragma once

#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/frustum.hpp"
#include "renderer/pass/render_pass.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace fjell {

class AccelStructureManager;

struct SsGiConfig {
    uint32_t probe_spacing{8};    // pixels per probe
    uint32_t rays_per_probe{64};  // hemisphere rays per probe per frame
    float max_trace_dist{50.0f};  // meters
    float hysteresis{0.90f};      // temporal blend factor (0.9 = 10% new, ~10 frame convergence)
    float intensity{1.0f};
    bool enabled{true};
};

/// The constants the SS-GI trace and resolve read.
struct GpuSsGiConstants {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 inv_view_proj{1.0f};
    glm::mat4 prev_view_proj{1.0f};
    glm::mat4 prev_inv_view_proj{1.0f};
    glm::vec3 camera_position;
    float max_trace_dist{20.0f};
    glm::vec2 resolution;
    uint32_t probe_spacing{16};
    uint32_t frame_index{0};
    glm::uvec2 probe_count;
    /// Metres between vertices of the coarsest geometry the rays trace
    /// against — terrain's ray mesh, the one surface traced at a lower
    /// resolution than it was shaded from. Rays start this much further out
    /// of a sloping surface, or they begin inside it. Zero without terrain.
    float trace_cell_size{0.0f};
    float pad1{0.0f};
    glm::vec4 sun_direction_intensity; // xyz = toward-light dir, w = intensity
    glm::vec4 sun_color;               // xyz = color, w = unused
};

class SsGiPass : public RenderPass {
public:
    /// The names each viewport's textures are exported under.
    static constexpr std::string_view OUTPUT_NAME = "ss_gi_output";
    static constexpr std::string_view PREV_DEPTH_NAME = "ss_gi_prev_depth";

    /// Per-viewport resolution-dependent state. The probe textures are the
    /// pass's own and rest where its compute shaders sample them; the output
    /// and the depth copy are exported to the frame graph.
    struct Images {
        /// What each ray brought back: probe_count.x wide, probe_count.y *
        /// rays_per_probe tall. The trace writes it, the resolve reads it.
        gpu::Owned<gpu::Texture> raw_radiance;
        /// Probe irradiance blended over frames, a texel per probe.
        gpu::Owned<gpu::Texture> resolved;
        /// Last frame's `resolved`, which this frame's blends onto.
        gpu::Owned<gpu::Texture> history;
        /// The indirect light the surfaces sample, at half the viewport.
        gpu::Owned<gpu::Texture> indirect;
        /// The scene depth as the last frame left it, the same size and
        /// format: the resolve drops history where the depth changed.
        gpu::Owned<gpu::Texture> prev_depth;

        glm::uvec2 indirect_extent{0, 0};
        glm::uvec2 probe_count{0, 0};
        uint32_t rays_per_probe{0};
        /// The upsample has written `indirect` since it was made; until it
        /// has, the surfaces sample the fallback.
        bool written{false};

        // The camera the history was resolved with, which the temporal
        // resolve reprojects through. Per-viewport, since each viewport has
        // its own camera history.
        glm::mat4 prev_view_proj{1.0f};
        glm::mat4 prev_inv_view_proj{1.0f};
        bool history_valid{false};

        void create(gpu::Device& gpu, const SsGiConfig& config, uint32_t screen_w, uint32_t screen_h);
        /// Releases the textures; the frames still reading them keep them
        /// until they are done.
        void destroy();
        /// Made again when the probe grid or the output size changes.
        void resize(gpu::Device& gpu, const SsGiConfig& config, uint32_t screen_w, uint32_t screen_h);
        /// Makes `prev_depth` again unless it already matches `scene_depth`.
        /// The render pipeline calls it before exporting the copy, for the
        /// target the viewport draws into this frame.
        void fit_prev_depth(gpu::Device& gpu, gpu::Texture scene_depth);
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

    /// Where it stops before upsampling into the indirect output, the
    /// output holds what it held.
    void record_into(const FrameContext& ctx, Images& imgs);

    /// Copy the scene depth into `imgs.prev_depth` for the next frame's
    /// resolve. SsGiDepthHistoryPass declares both, which orders it after
    /// this frame's resolve has read the last copy.
    void record_depth_history(const FrameContext& ctx, Images& imgs);

    /// Whether the pass records at all: enabled, with its trace pipeline.
    [[nodiscard]] bool active() const {
        return config_.enabled && trace_pipeline_;
    }

    void set_config(const SsGiConfig& config) { config_ = config; }
    [[nodiscard]] SsGiConfig& config() { return config_; }
    [[nodiscard]] const SsGiConfig& config() const { return config_; }

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }

    /// The box this pass's rays can reach from `camera`, for the scene's
    /// acceleration structure: a cube of the trace distance around it, as
    /// for reflections. Probes sit on visible surfaces and trace from there,
    /// so rays that start far from the camera can leave the cube.
    [[nodiscard]] AABB trace_reach_bounds(const glm::vec3& camera) const {
        return {camera - glm::vec3(config_.max_trace_dist), camera + glm::vec3(config_.max_trace_dist)};
    }

    /// The coarsest cell in the traced geometry, in metres — terrain's ray
    /// mesh spacing. Rays start further out of a sloping surface by this
    /// much, or they begin inside the mesh they are about to trace. Zero when
    /// no terrain is loaded, which puts rays back on the flat-ground minimum.
    void set_trace_cell_size(float m) { trace_cell_size_ = m; }
    [[nodiscard]] float trace_cell_size() const { return trace_cell_size_; }

    void set_sun_light(glm::vec3 direction, glm::vec3 color, float intensity) {
        sun_direction_ = direction;
        sun_color_ = color;
        sun_intensity_ = intensity;
    }

    [[nodiscard]] gpu::Sampler linear_sampler() const { return linear_sampler_; }

private:
    [[nodiscard]] GpuSsGiConstants constants(const Images& imgs, const FrameContext& ctx) const;

    SsGiConfig config_;
    uint32_t frame_counter_{0};

    // Cached sun light data (set via set_sun_light before recording)
    glm::vec3 sun_direction_{0.0f, -1.0f, 0.0f};
    glm::vec3 sun_color_{1.0f};
    float sun_intensity_{1.5f};

    /// Traces with ray queries; null on a device without them.
    gpu::Owned<gpu::ComputePipeline> trace_pipeline_;
    gpu::Owned<gpu::ComputePipeline> resolve_pipeline_;
    gpu::Owned<gpu::ComputePipeline> upsample_pipeline_;

    gpu::Sampler linear_sampler_;
    gpu::Sampler nearest_sampler_;

    AccelStructureManager* accel_manager_{nullptr};
    float trace_cell_size_{0.0F};
};

/// Copies the scene depth into SS-GI's copy of it for the next frame. A node
/// of its own so the graph moves the scene depth into the copy's layout and
/// back to where its next reader wants it, and orders the copy after SS-GI
/// has read the last one.
class SsGiDepthHistoryPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    SsGiPass* core_{nullptr};
};

} // namespace fjell

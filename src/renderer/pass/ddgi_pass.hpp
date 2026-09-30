#pragma once

#include "renderer/pass/render_pass.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/frustum.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace fjell {

class AccelStructureManager;
class SceneSdfPass;
class SkyCubemapPass;

enum class DdgiBackend : uint8_t {
    sdf_fallback,
    ray_traced,
};

/// Diagnostic views that replace the DDGI pass's indirect output. Values
/// mirror the DDGI_DEBUG_* constants in ddgi_sample_irradiance.comp.
///
/// These are only readable with the lighting channel set to raw DDGI: the
/// surface composite multiplies the pass output by albedo and kD, so a
/// diagnostic color otherwise arrives tinted by whatever it lands on, and
/// a dark surface makes any of them look like "no data". The settings
/// panel switches the channel automatically when one of these is picked.
enum class DdgiDebugMode : uint32_t {
    Off = 0,
    /// The cage cell each pixel lands in, hashed to a color. Artifacts
    /// that align with these cells come from the cage or its probes.
    GridCell = 1,
    /// Nearest probe's status: green active, red off (buried in
    /// geometry), blue asleep.
    ProbeStatus = 2,
    /// Total cage weight before normalization, log-scaled. The cage
    /// divides by this, so darkening caused by weights collapsing shows
    /// here while darkening from probe irradiance does not.
    CageWeight = 3,
    /// Normal output with the Chebyshev visibility term forced to 1. If
    /// an artifact disappears, the visibility test is rejecting probes.
    NoVisibility = 4,
    /// Distance to the nearest cage corner — draws the probe lattice
    /// itself, so a suspected pattern can be checked against where the
    /// probes actually sit.
    ProbeDistance = 5,
    /// Distance to the probe grid's boundary: magenta outside it, red at
    /// the edge, green well inside. Tells you whether an artifact lines up
    /// with where the grid stops — and so with where the acceleration
    /// structure stops covering what probes trace against.
    TraceReach = 6,
    /// Nearest probe's irradiance, read directly with no cage and no
    /// weights. Flat-shaded per cell, so a pattern that survives here is
    /// in the stored probe data rather than in the interpolation.
    RawProbe = 7,
    /// Red: fraction of cage probes whose Chebyshev weight hit the 0.05
    /// floor. Green: fraction that were tested at all. Isolates the only
    /// weight term that varies within a cell.
    ChebReject = 8,
};

/// DDGI probe grid configuration.
/// The grid is fitted to the scene bounds and fitted again whenever content
/// reaches outside it (VulkanContext does this, through refit_grid()). Every
/// fit moves the probes to new world positions, which discards everything
/// they have accumulated.
struct DdgiConfig {
    /// Cubic: the grid is fitted to scene bounds, so a shorter Y axis does
    /// not mean finer vertical detail — it means the same scene height is
    /// covered by fewer probes, and vertical spacing grows to match.
    glm::ivec3 probe_counts{16, 16, 16};
    glm::vec3 probe_spacing{2.0f};
    glm::vec3 grid_position{-7.0f, -1.0f, -7.0f};  // replaced by the first fit
    float hysteresis{0.95f};
    int probe_rays{128};  // at most DdgiPass::MAX_PROBE_RAYS
    // Octahedral texel resolution of one probe's own irradiance map — the
    // full sphere of incoming directions at that probe is represented by
    // this many texels per side. Cross-probe blending (the 8-probe
    // trilinear cage in ddgi_sample_irradiance.comp) is already smooth;
    // this resolution is what determines how much real angular detail
    // exists WITHIN one probe before that blend runs. At 6 it's visible as
    // a faint grid pattern on large, flat, textureless, brightly-lit
    // surfaces (nothing else masks the under-sampling) — see
    // Plans/parked/lighting/gi-shadow-artifact-investigation.md. 8 is a moderate first
    // step: ~78% more per-probe irradiance-update compute
    // (proportional to side²), and atlas VRAM that scales with the probe
    // count — cheap on both fronts, revisit higher if still visible.
    int irradiance_side_length{8};  // at most DdgiPass::MAX_ATLAS_SIDE
    // Visibility (mean depth + variance for Chebyshev occlusion testing)
    // is lower-frequency than full directional irradiance color, so it
    // doesn't need to track the same resolution bump — left at 6.
    int visibility_side_length{6};  // at most DdgiPass::MAX_ATLAS_SIDE
    float self_shadow_bias{0.3f};
    // Sample-position bias as a fraction of probe spacing. Normal-leaning so
    // the sample lifts off the surface rather than sliding along it at grazing
    // angles (which would skate across probe cells). view_bias is small.
    // Lifts the sample toward the probe so dist_to_biased lands inside
    // mean_dist and the Chebyshev branch is skipped. This is what escapes
    // the degenerate case on a flat surface (probe sees a perfectly
    // consistent depth, variance ~0, weight floors at 0.05 exactly where
    // the trilinear weight peaks — a dark spot per probe).
    //
    // It has to be the NORMAL term that does this, not the view term: the
    // view direction tilts toward horizontal as a surface recedes, so its
    // vertical component falls off with distance. That produced dots which
    // were faint near the camera and dark far away.
    float normal_bias{0.4f};
    // Scaled by distance to the camera, fading out within one cell. This
    // is what keeps the Chebyshev test off the degenerate case where a
    // probe over a flat surface records a perfectly consistent depth, the
    // variance is zero, and the weight floors at 0.05 exactly where the
    // trilinear weight peaks — a dark spot at every probe. Lumen uses an
    // equivalent 0.6 (0.8 * 0.75) against 0.15 (0.2 * 0.75) for normal;
    // the view term is deliberately the larger of the two.
    float view_bias{0.6f};
    float max_probe_offset{0.5f};
    // How strong bounced light is: scales what a probe ray brings back from
    // a hit (the sun, a local light, the bounce off the field), not the sky a
    // miss brings, which EnvironmentState::sky_light_intensity() sets. It
    // compounds per bounce: 0.65 gives the first bounce 0.65, the second
    // about 0.42.
    float gi_intensity{1.0f};
    // Per-bounce energy retained in the multi-bounce feedback loop
    // (ddgi_probe_trace_rt.comp adds sample_ddgi_irradiance() * this back
    // into each ray hit's radiance). The feedback is a geometric series —
    // steady-state total indirect settles at direct_light / (1 - this), so
    // 0.8 converges to 5x the direct term. That's enough to keep pushing a
    // probe's stored irradiance above the 30.0 clamp on bright, saturated
    // surfaces (bright white light on a bright white material) every frame,
    // not rarely — a visible clamped plateau instead of the intended subtle
    // multi-bounce boost. 0.5 -> 2x steady-state, still a clearly visible
    // bounce contribution without living at the clamp ceiling for common
    // bright content.
    float infinite_bounces_mult{0.5f};
    // Probes traced+updated per frame, round-robin (0 = all). Spreading the
    // update over several frames cuts the per-frame ray cost; hysteresis
    // hides the staggering. Also caps the radiance image's height, since
    // the trace writes one row per probe in the current batch.
    int probes_per_frame_batch{512};
    bool half_res_output{true};
    bool enabled{true};
    /// Diagnostic view replacing the indirect output. Runtime only, never
    /// saved with the project.
    DdgiDebugMode debug_mode{DdgiDebugMode::Off};
};

class DdgiPass : public RenderPass {
public:
    /// The names each viewport's probe field is exported under: the three
    /// atlases, which rest where compute shaders and the editor sample them,
    /// and the probe status. RT reflections light their hits from them.
    static constexpr std::string_view IRRADIANCE_NAME = "ddgi_irradiance";
    static constexpr std::string_view VISIBILITY_NAME = "ddgi_visibility";
    static constexpr std::string_view PROBE_OFFSET_NAME = "ddgi_probe_offset";
    static constexpr std::string_view PROBE_STATUS_NAME = "ddgi_probe_status";
    static constexpr std::string_view OUTPUT_NAME = "ddgi_indirect";

    /// Where the atlases rest between the passes that declare them.
    static constexpr gpu::AccessSet ATLAS_REST = gpu::Access::sampled_compute | gpu::Access::sampled_fragment;

    /// Per-viewport resolution-dependent state.
    struct Images {
        /// What this frame's batch of probes traced: probe_rays wide, one row
        /// per probe traced this frame. The trace writes it, the update reads it.
        gpu::Owned<gpu::Texture> radiance;
        /// The probe field: irradiance (RGBA16F), visibility (RG16F: mean hit
        /// distance and its square) and each probe's offset (RGBA16F,
        /// probes_xy x probes_z). Made cleared.
        gpu::Owned<gpu::Texture> irradiance;
        gpu::Owned<gpu::Texture> visibility;
        gpu::Owned<gpu::Texture> probe_offset;
        /// A uint per probe; made with every probe untraced.
        gpu::Owned<gpu::Buffer> probe_status;

        /// Constants, written by the CPU every frame, so one per frame in
        /// flight: a frame still tracing must not see the next frame's grid
        /// origin or batch cursor. RT reflections read the slot's copy.
        std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> constants;

        // The point and spot lights that reach the grid, as the tracers read
        // them (DDGI_LIGHT_LIST_BYTES): filled on the GPU each frame by the
        // light select, one per frame in flight.
        std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> light_list;

        /// The indirect light the surfaces sample: screen-res or half-res.
        gpu::Owned<gpu::Texture> indirect;
        glm::uvec2 indirect_extent{0, 0};
        /// The sample has written `indirect` since it was made; until it
        /// has, the surfaces sample the fallback.
        bool indirect_written{false};

        /// Grid generation this viewport last discarded its atlases at.
        uint32_t reset_generation_{0};

        /// Whether this viewport has recorded a frame yet. Per-viewport for
        /// the same reason as reset_generation_: record_into() runs once per
        /// viewport, so a pass-level flag would be consumed by whichever
        /// records first.
        bool first_frame_{true};

        // Atlas dimensions (computed from config)
        int32_t irr_width{0};
        int32_t irr_height{0};
        int32_t vis_width{0};
        int32_t vis_height{0};
        int32_t total_probes{0};
        /// Rows in the radiance texture — the cap on probes traceable in one
        /// frame, since the trace writes one row per probe.
        int32_t radiance_rows{0};

        // Round-robin batch cursor — per-viewport, since each viewport owns its
        // own radiance texture and atlases (a shared cursor would desync them).
        int32_t probe_update_offset{0};

        /// The sky cubemap the probes last integrated, so a sky that changes
        /// identity (an HDRI loaded or cleared) discards what they hold.
        gpu::Texture last_sky{};

        gpu::Device* device{nullptr};

        // ImGui debug textures
        VkDescriptorSet indirect_imgui_desc{VK_NULL_HANDLE};
        VkDescriptorSet irradiance_imgui_desc{VK_NULL_HANDLE};

        [[nodiscard]] VkDescriptorSet indirect_imgui_texture(gpu::Sampler sampler);
        [[nodiscard]] VkDescriptorSet irradiance_imgui_texture(gpu::Sampler sampler);

        void create(gpu::Device& gpu, const DdgiConfig& config, uint32_t screen_w, uint32_t screen_h);
        /// Releases everything; the frames still reading the textures keep
        /// them until they are done, but the editor's views go at once.
        void destroy();
        /// Everything is made again when the probe count changes, which
        /// discards what the probes hold; a new screen size makes only the
        /// indirect output again.
        void resize(gpu::Device& gpu, const DdgiConfig& config, uint32_t screen_w, uint32_t screen_h);
        /// Makes everything again for `config`, discarding what the probes
        /// hold, while frames may still be using what it replaces.
        void remake(gpu::Device& gpu, const DdgiConfig& config, uint32_t screen_w, uint32_t screen_h);

        /// Drops the editor's views at once: nothing may still be drawing them.
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;

    /// Record into externally-provided images. Where it stops before
    /// sampling into the indirect output, the output holds what it held.
    void record_into(const FrameContext& ctx, Images& imgs);

    /// Move the probe grid and discard everything accumulated in it.
    ///
    /// A probe's irradiance only means anything for the world position it
    /// was traced at, so any change to grid_position or probe_spacing
    /// invalidates the whole atlas. Callers use this rather than writing
    /// config() directly, so the discard cannot be forgotten.
    void refit_grid(const glm::vec3& grid_position, const glm::vec3& probe_spacing) {
        config_.grid_position = grid_position;
        config_.probe_spacing = probe_spacing;
        ++grid_generation_;
    }

    /// Discard accumulated probe irradiance without touching the grid fit.
    /// Probe atlases integrate the sky, so a sky that changes identity (an
    /// HDRI finishing its async load, or being cleared) leaves every probe
    /// holding radiance gathered from the previous one.
    void invalidate_probe_lighting() { ++grid_generation_; }

    void set_config(const DdgiConfig& config) { config_ = config; }
    [[nodiscard]] DdgiConfig& config() { return config_; }
    [[nodiscard]] const DdgiConfig& config() const { return config_; }
    [[nodiscard]] gpu::Sampler linear_sampler() const { return linear_sampler_; }

    void set_backend(DdgiBackend b) { backend_ = b; }
    [[nodiscard]] DdgiBackend backend() const { return backend_; }

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }

    /// World-space bounds of the scene SDF volume, used by both traces to
    /// test whether a probe is buried and by the SDF trace to march. Kept current by the owner since the
    /// SDF volume follows the scene every frame while the probe grid moves only
    /// when content escapes it.
    void set_sdf_bounds(const glm::vec3& min, const glm::vec3& max) {
        sdf_volume_min_ = min;
        sdf_volume_max_ = max;
    }

    /// Push constants for the SDF trace shader.
    struct TracePushConstants {
        glm::vec3 light_direction;
        float light_intensity;
        glm::vec3 light_color;
        float sdf_resolution;
        glm::vec3 sdf_volume_min;
        /// Changes every frame, so a probe hit takes a different local light
        /// each time it is traced.
        uint32_t random_seed;
        glm::vec3 sdf_volume_max;
        /// How brightly the sky lights the world, which a miss ray brings.
        float sky_intensity;
        /// How strong bounced light is (gi_intensity), on what a hit brings.
        float gi_intensity;
    };

    /// Push constants for the RT trace shader. Laid out to match the shader's
    /// std430 push-constant block: each vec3 is 16-byte aligned, so the two
    /// trailing pads keep sdf_volume_min/max on 16-byte boundaries.
    struct RtTracePushConstants {
        glm::vec3 light_direction;
        float light_intensity;
        glm::vec3 light_color;
        float max_trace_dist;
        glm::vec3 sdf_volume_min;
        /// Changes every frame, so a probe hit takes a different local light
        /// each time it is traced.
        uint32_t random_seed;
        glm::vec3 sdf_volume_max;
        /// How brightly the sky lights the world, which a miss ray brings.
        float sky_intensity;
        /// How strong bounced light is (gi_intensity), on what a hit brings.
        float gi_intensity;
    };

    /// Max ray distance used in RT trace push constants.
    static constexpr float MAX_TRACE_DIST = 40.0f;

    /// What ddgi_probe_update.comp holds in shared memory for one probe
    /// (DDGI_MAX_PROBE_RAYS and DDGI_MAX_ATLAS_SIDE there): the most rays a
    /// probe traces, and the widest octahedral map either atlas keeps.
    static constexpr int MAX_PROBE_RAYS = 256;
    static constexpr int MAX_ATLAS_SIDE = 16;

    /// Probe grid bounds expanded by max trace distance — objects outside
    /// this volume can never be hit by DDGI rays, so it is the reach DDGI
    /// declares for the acceleration structure.
    [[nodiscard]] AABB trace_reach_bounds() const;

private:
    /// Writes this frame's constants into the slot's buffer, and returns
    /// them.
    GpuDdgiConstants update_constants(Images& imgs, const FrameContext& ctx);
    /// Fills this slot's light list with the lights that reach the grid, from
    /// the world light buffer the global set binds.
    void record_light_select(const FrameContext& ctx, Images& imgs);

    DdgiConfig config_;
    uint32_t frame_counter_{0};

    // Scene light state (set by prepare_frame)
    glm::vec3 light_direction_{-0.3f, -1.0f, -0.5f};
    float light_intensity_{1.5f};
    glm::vec3 light_color_{1.0f, 1.0f, 0.95f};
    // EnvironmentState::sky_light_intensity(): the sky the probes see
    float sky_intensity_{1.0f};

    // Scene SDF volume bounds (set by owner; used for the RT burial test)
    glm::vec3 sdf_volume_min_{-10.0f};
    glm::vec3 sdf_volume_max_{10.0f};

    /// The SDF tracer (set 1 the bindless material set a hit's surface is
    /// read from), the probe update, the screen sample, and the light select
    /// (set 0 the global set, whose light buffer it reads).
    gpu::Owned<gpu::ComputePipeline> trace_pipeline_;
    gpu::Owned<gpu::ComputePipeline> probe_update_pipeline_;
    gpu::Owned<gpu::ComputePipeline> sample_pipeline_;
    gpu::Owned<gpu::ComputePipeline> light_select_pipeline_;

    gpu::Sampler linear_sampler_;
    /// Bumped by refit_grid(). Each viewport compares this against the
    /// generation it last reset at, so every viewport discards its own
    /// atlases exactly once per refit — record_into() runs per viewport,
    /// so a single shared flag would be consumed by whichever recorded
    /// first and leave the rest holding stale irradiance.
    uint32_t grid_generation_{0};

    // The scene SDF, which the SDF tracer marches and whose index volume and
    // object table colour its hits; the ray-traced tracer tests burial in it.
    const SceneSdfPass* scene_sdf_{nullptr};
    /// Where a probe ray that misses finds the sky.
    const SkyCubemapPass* sky_{nullptr};

    // RT backend
    DdgiBackend backend_{DdgiBackend::sdf_fallback};
    AccelStructureManager* accel_manager_{nullptr};

    /// The ray-traced tracer, tracing with ray queries (set 1 the bindless
    /// material set); null on a device without them.
    gpu::Owned<gpu::ComputePipeline> rt_trace_pipeline_;
};

} // namespace fjell

#pragma once

#include "renderer/pass/render_pass.hpp"
#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace fjell {

class AccelStructureManager;

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
/// The grid is auto-fitted to the scene SDF bounds once on first frame, then
/// frozen. Changing grid_position or probe_spacing at runtime invalidates all
/// accumulated irradiance (probes shift to new world positions). For streaming
/// or procedural worlds, set bounds before the first frame or use a scrolling
/// grid (not yet implemented).
struct DdgiConfig {
    /// Cubic: the grid is fitted to scene bounds, so a shorter Y axis does
    /// not mean finer vertical detail — it means the same scene height is
    /// covered by fewer probes, and vertical spacing grows to match.
    glm::ivec3 probe_counts{16, 16, 16};
    glm::vec3 probe_spacing{2.0f};
    glm::vec3 grid_position{-7.0f, -1.0f, -7.0f};  // auto-fitted on first frame
    float hysteresis{0.95f};
    int probe_rays{128};
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
    int irradiance_side_length{8};
    // Visibility (mean depth + variance for Chebyshev occlusion testing)
    // is lower-frequency than full directional irradiance color, so it
    // doesn't need to track the same resolution bump — left at 6.
    int visibility_side_length{6};
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
    float gi_intensity{1.0f};
    // Per-bounce energy retained in the multi-bounce feedback loop
    // (ddgi_probe_trace_rt.rchit adds sample_ddgi_irradiance() * this back
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
    /// Blend each probe's irradiance with its face-adjacent neighbours
    /// before sampling. Without it the trilinear cage exposes every
    /// disagreement between neighbouring probes as a visible lattice.
    /// Lumen runs the equivalent pass by default.
    bool spatial_filter{true};
    /// Radians. A neighbour whose hit point lies further off this probe's
    /// cone direction is looking at different geometry and is rejected.
    /// Larger values filter more and leak more; Lumen defaults to 0.2.
    float filter_max_hit_angle{0.2f};
    bool half_res_output{true};
    bool enabled{true};
    /// Diagnostic view replacing the indirect output. Runtime only, never
    /// saved with the project.
    DdgiDebugMode debug_mode{DdgiDebugMode::Off};
};

/// GPU-side constants UBO (std140 layout).
struct GpuDdgiConstants {
    glm::vec3 probe_grid_position;
    float probe_sphere_scale{0.1f};

    glm::vec3 probe_spacing;
    float max_probe_offset;

    glm::vec3 reciprocal_spacing;
    float self_shadow_bias;

    glm::ivec3 probe_counts;
    uint32_t debug_options{0};

    int32_t irr_tex_w{0};
    int32_t irr_tex_h{0};
    int32_t irr_side{0};
    int32_t probe_rays{0};

    int32_t vis_tex_w{0};
    int32_t vis_tex_h{0};
    int32_t vis_side{0};
    int32_t probe_update_offset{0};

    int32_t probe_update_count{0};
    float hysteresis{0.97f};
    float infinite_bounces_mult{0.0f};
    float normal_bias{0.0f};   // sample bias along the surface normal (× spacing)

    glm::mat4 random_rotation{1.0f};

    // Camera data for sampling pass
    glm::vec3 camera_position;
    float pad1{0.0f};

    glm::mat4 inv_view_proj{1.0f};

    glm::vec2 resolution;
    float view_bias{0.0f};     // sample bias toward the camera (× spacing)
    float pad3{0.0f};
};

class DdgiPass : public RenderPass {
public:
    /// Per-viewport resolution-dependent image state.
    struct Images {
        // Radiance: probe_rays x total_probes, R16G16B16A16_SFLOAT
        VkImage radiance_image{VK_NULL_HANDLE};
        VmaAllocation radiance_memory{VK_NULL_HANDLE};
        VkImageView radiance_view{VK_NULL_HANDLE};

        // Irradiance atlas: R16G16B16A16_SFLOAT
        VkImage irradiance_image{VK_NULL_HANDLE};
        VmaAllocation irradiance_memory{VK_NULL_HANDLE};
        VkImageView irradiance_view{VK_NULL_HANDLE};

        // Spatial-filter destination for the irradiance atlas. The filter
        // gathers a probe's neighbours, so it needs a separate target.
        VkImage irradiance_filtered_image{VK_NULL_HANDLE};
        VmaAllocation irradiance_filtered_memory{VK_NULL_HANDLE};
        VkImageView irradiance_filtered_view{VK_NULL_HANDLE};

        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> filter_set{};

        /// Which atlas the consumers are currently bound to. The sample and
        /// RT sets are written once at init, so toggling the filter has to
        /// rewrite that one binding or the change never reaches the GPU.
        bool bound_to_filtered{false};

        /// Grid generation this viewport last discarded its atlases at.
        uint32_t reset_generation_{0};

        /// Whether this viewport has recorded a frame yet. Per-viewport for
        /// the same reason as reset_generation_: record_into() runs once per
        /// viewport, so a pass-level flag is consumed by whichever records
        /// first, and the rest transition their images from GENERAL when
        /// those images have never been transitioned out of UNDEFINED.
        bool first_frame_{true};

        // Visibility atlas: R16G16_SFLOAT
        VkImage visibility_image{VK_NULL_HANDLE};
        VmaAllocation visibility_memory{VK_NULL_HANDLE};
        VkImageView visibility_view{VK_NULL_HANDLE};

        // Probe offsets: R16G16B16A16_SFLOAT
        VkImage probe_offset_image{VK_NULL_HANDLE};
        VmaAllocation probe_offset_memory{VK_NULL_HANDLE};
        VkImageView probe_offset_view{VK_NULL_HANDLE};

        // Indirection: grid coord -> probe slot, R32_UINT 3D, probe_counts
        // texels. Double-buffered — the reallocation pass reads [1 - current]
        // (the mapping from before this shift) and writes [current].
        VkImage indirection_image[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VmaAllocation indirection_memory[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageView indirection_view[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        int indirection_current{0};

        // Slot -> grid coord (xyz) + flags (w), one uvec4 per probe slot.
        // The inverse of the indirection texture, for dispatches that run
        // per-slot and need to know which cell the slot currently holds.
        std::unique_ptr<Buffer> slot_grid_buffer;

        // Free list consumed by the reallocation pass: leading counter,
        // then one slot index per probe.
        std::unique_ptr<Buffer> free_list_buffer;

        // Probe status SSBO (uint per probe)
        std::unique_ptr<Buffer> probe_status_buffer;

        // Constants UBO, host-mapped and written every frame, so one per
        // frame in flight: a frame still tracing must not see the next
        // frame's grid origin or batch cursor.
        std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> constants_ubo;

        // Indirect output: screen-res or half-res, R16G16B16A16_SFLOAT
        VkImage indirect_image{VK_NULL_HANDLE};
        VmaAllocation indirect_memory{VK_NULL_HANDLE};
        VkImageView indirect_view{VK_NULL_HANDLE};
        /// The indirect output as the GPU interface's texture.
        gpu::Owned<gpu::Texture> indirect_texture;
        VkExtent2D indirect_extent{};

        // Atlas dimensions (computed from config)
        int32_t irr_width{0};
        int32_t irr_height{0};
        int32_t vis_width{0};
        int32_t vis_height{0};
        int32_t total_probes{0};
        /// Rows in the radiance image — the cap on probes traceable in one
        /// frame, since the trace writes one row per probe.
        int32_t radiance_rows{0};

        // Round-robin batch cursor — per-viewport, since each viewport owns its
        // own radiance image and atlases (a shared cursor would desync them).
        int32_t probe_update_offset{0};

        bool images_need_init{true};
        /// The indirect image was just (re)created and is still in UNDEFINED
        /// layout. Tracked apart from first_frame_ because a screen resize
        /// recreates only this image: the probe atlases keep their contents
        /// and their GENERAL layout, and transitioning them from UNDEFINED
        /// would discard converged irradiance on every window resize.
        bool indirect_needs_init{true};
        /// The pending descriptor rewrite came from a screen resize, which
        /// leaves probe data valid. Distinguishes it from a fresh create().
        bool resized_only_{false};

        // Cached sky cubemap identity — if the registry entry swaps
        // (e.g. HDRI loaded/cleared), bump images_need_init to rewrite
        // the static cubemap binding on trace_set / rt_trace_set.
        VkImageView last_sky_view{VK_NULL_HANDLE};
        VkSampler last_sky_sampler{VK_NULL_HANDLE};

        // Cached depth/normal views on the sample set — only change on resize,
        // so skip the per-frame descriptor rewrite when they're unchanged.
        VkImageView last_depth_view{VK_NULL_HANDLE};
        VkImageView last_normal_view{VK_NULL_HANDLE};

        // ImGui debug textures
        VkDescriptorSet indirect_imgui_desc{VK_NULL_HANDLE};
        VkDescriptorSet irradiance_imgui_desc{VK_NULL_HANDLE};

        [[nodiscard]] VkDescriptorSet indirect_imgui_texture(VkSampler sampler);
        [[nodiscard]] VkDescriptorSet irradiance_imgui_texture(VkSampler sampler);

        // Descriptor sets — written by DdgiPass after Images::create(). One
        // of each per frame in flight: every set binds that slot's constants
        // UBO, and the RT trace set also binds the slot's TLAS and
        // per-instance table. Rewriting one shared set would hand a
        // still-pending frame this frame's slot, a TLAS built three frames
        // ago against an instance table just overwritten with a different
        // layout. Bindings that are the same for every slot are written into
        // all of them together.
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> trace_set{};     // SDF trace
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> rt_trace_set{};  // RT trace
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> update_set{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sample_set{};

        void create(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     const DdgiConfig& config,
                     uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(gpu::Device& gpu, VkDevice device, VmaAllocator allocator,
                     const DdgiConfig& config,
                     uint32_t screen_w, uint32_t screen_h);

        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    /// Record into externally-provided images. Where it stops before
    /// sampling into the indirect output, the output holds what it held.
    void record_into(const FrameContext& ctx, Images& imgs);

    /// Build acceleration structures before command buffer recording.
    /// Must be called outside of command buffer recording (uses synchronous GPU submits).
    void prepare_accel_structures(const FrameContext& ctx);

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
    [[nodiscard]] VkSampler linear_sampler() const { return linear_sampler_; }

    void set_backend(DdgiBackend b) { backend_ = b; }
    [[nodiscard]] DdgiBackend backend() const { return backend_; }

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }
    void set_particle_system(class ParticleSystem* ps) { particle_system_ = ps; }

    /// World-space bounds of the scene SDF volume, used by the RT trace to
    /// test whether a probe is buried. Kept current by the owner since the
    /// SDF volume can move while the probe grid stays frozen.
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
        float pad0;
        glm::vec3 sdf_volume_max;
        float pad1;
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
        float pad0;
        glm::vec3 sdf_volume_max;
        float pad1;
    };

    /// Push constants for the irradiance spatial filter.
    struct FilterPushConstants {
        float max_hit_angle;
        uint32_t border_phase;
    };

    /// Max ray distance used in RT trace push constants.
    static constexpr float MAX_TRACE_DIST = 40.0f;

    /// Probe grid bounds expanded by max trace distance — objects outside
    /// this volume can never be hit by DDGI rays, so it is also the volume
    /// the gather keeps off-screen geometry for.
    [[nodiscard]] AABB trace_reach_bounds() const;

private:
    void update_constants(Images& imgs, const FrameContext& ctx);

    Device* device_{nullptr};
    VkDevice vk_device_{VK_NULL_HANDLE};
    DdgiConfig config_;
    uint32_t frame_counter_{0};

    // Scene light state (set by prepare_frame)
    glm::vec3 light_direction_{-0.3f, -1.0f, -0.5f};
    float light_intensity_{1.5f};
    glm::vec3 light_color_{1.0f, 1.0f, 0.95f};

    // Scene SDF volume bounds (set by owner; used for the RT burial test)
    glm::vec3 sdf_volume_min_{-10.0f};
    glm::vec3 sdf_volume_max_{10.0f};

    // Compute pipelines
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout update_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout update_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline irradiance_update_pipeline_{VK_NULL_HANDLE};
    VkPipeline visibility_update_pipeline_{VK_NULL_HANDLE};
    VkPipeline probe_offset_pipeline_{VK_NULL_HANDLE};
    VkPipeline probe_status_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout filter_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout filter_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline irradiance_filter_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout sample_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout sample_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline sample_pipeline_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    /// Bumped by refit_grid(). Each viewport compares this against the
    /// generation it last reset at, so every viewport discards its own
    /// atlases exactly once per refit — record_into() runs per viewport,
    /// so a single shared flag would be consumed by whichever recorded
    /// first and leave the rest holding stale irradiance.
    uint32_t grid_generation_{0};

    // RT backend
    DdgiBackend backend_{DdgiBackend::sdf_fallback};
    AccelStructureManager* accel_manager_{nullptr};
    class ParticleSystem* particle_system_{nullptr};

    VkDescriptorSetLayout rt_trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout rt_trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline rt_pipeline_{VK_NULL_HANDLE};

    // Shader binding table
    std::unique_ptr<Buffer> sbt_buffer_;
    VkStridedDeviceAddressRegionKHR sbt_raygen_{};
    VkStridedDeviceAddressRegionKHR sbt_miss_{};
    VkStridedDeviceAddressRegionKHR sbt_hit_{};
    VkStridedDeviceAddressRegionKHR sbt_callable_{};
};

} // namespace fjell

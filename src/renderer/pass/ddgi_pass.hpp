#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "core/math/ddgi_clipmap.hpp"

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

/// DDGI probe grid configuration.
/// The grid is a camera-centered clipmap: it covers a fixed volume around the
/// camera and follows it, snapping to its own cell grid so it shifts in whole
/// cells and only when the camera crosses a cell boundary. Probes that still
/// cover the same world cell after a shift keep their accumulated irradiance;
/// only the newly-uncovered ones retrace. Changing probe_counts or
/// probe_spacing resizes the level and discards everything, since both change
/// which world position each probe stands for.
struct DdgiConfig {
    // Cube-shaped, matching Lumen's clipmap levels (one ClipmapResolution
    // for all three axes). The old 16x8x16 came from fitting the grid to
    // scene bounds, where a squat box suited a floor-based scene; a
    // camera-centered level has to reach as far above and below the camera
    // as it does sideways, or tall geometry falls out of coverage.
    glm::ivec3 probe_counts{16, 16, 16};
    glm::vec3 probe_spacing{2.0f};
    glm::vec3 grid_position{-7.0f, -1.0f, -7.0f};  // driven by the clipmap
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
    // Plans/gi-shadow-artifact-investigation.md. 8 is a moderate first
    // step: ~78% more per-probe irradiance-update compute
    // (proportional to side²), a few hundred KB more atlas VRAM across
    // 2048 probes — cheap on both fronts, revisit higher if still visible.
    int irradiance_side_length{8};
    // Visibility (mean depth + variance for Chebyshev occlusion testing)
    // is lower-frequency than full directional irradiance color, so it
    // doesn't need to track the same resolution bump — left at 6.
    int visibility_side_length{6};
    float self_shadow_bias{0.3f};
    // Sample-position bias as a fraction of probe spacing. Normal-leaning so
    // the sample lifts off the surface rather than sliding along it at grazing
    // angles (which would skate across probe cells). view_bias is small.
    float normal_bias{0.25f};
    float view_bias{0.08f};
    float max_probe_offset{0.5f};
    float gi_intensity{1.0f};
    float emissive_gi_boost{4.0f};
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
    // 2048-probe update over several frames cuts the per-frame ray cost;
    // hysteresis hides the staggering.
    int probes_per_frame_batch{512};
    bool half_res_output{true};
    bool enabled{true};
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

        // Constants UBO (host-mapped, updated each frame)
        std::unique_ptr<Buffer> constants_ubo;

        // Indirect output: screen-res or half-res, R16G16B16A16_SFLOAT
        VkImage indirect_image{VK_NULL_HANDLE};
        VmaAllocation indirect_memory{VK_NULL_HANDLE};
        VkImageView indirect_view{VK_NULL_HANDLE};
        VkExtent2D indirect_extent{};

        // Atlas dimensions (computed from config)
        int32_t irr_width{0};
        int32_t irr_height{0};
        int32_t vis_width{0};
        int32_t vis_height{0};
        int32_t total_probes{0};

        // Round-robin batch cursor — per-viewport, since each viewport owns its
        // own radiance image and atlases (a shared cursor would desync them).
        int32_t probe_update_offset{0};

        bool images_need_init{true};

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

        // Descriptor sets — written by DdgiPass after Images::create()
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        VkDescriptorSet trace_set{VK_NULL_HANDLE};     // SDF trace
        VkDescriptorSet rt_trace_set{VK_NULL_HANDLE};  // RT trace
        VkDescriptorSet update_set{VK_NULL_HANDLE};
        VkDescriptorSet sample_set{VK_NULL_HANDLE};
        VkDescriptorSet alloc_set{VK_NULL_HANDLE};  // clipmap reallocation

        // False until the reallocation pass has written both indirection
        // images at least once; before that their contents are undefined
        // and they have never been transitioned out of UNDEFINED.
        bool indirection_initialized{false};

        // This viewport's clipmap level. Per-viewport, not per-pass: each
        // viewport renders from its own camera and owns its own indirection
        // texture and probe atlases, so it has to recenter on its own camera
        // — a shared level would place probes around one camera while
        // another viewport sampled them. Matches Lumen, which stores the
        // whole FRadianceCacheState (clipmap origins and the probe
        // indirection texture together) in per-view state.
        DdgiClipmapLevel level;

        // The level origin this viewport's indirection texture was last
        // built for. Differs from level.snapped_origin exactly when the
        // texture still has to catch up.
        glm::vec3 indirection_origin{0.0f};

        // Set during recentering when this viewport's indirection texture is
        // out of date, so both the constants (which must widen the update to
        // every probe) and the dispatch agree on it within one frame.
        bool reallocated_this_frame{false};

        void create(VkDevice device, VmaAllocator allocator,
                     const DdgiConfig& config,
                     uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);
        void resize(VkDevice device, VmaAllocator allocator,
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

    /// Record into externally-provided images.
    void record_into(const FrameContext& ctx, Images& imgs);

    /// Build acceleration structures before command buffer recording.
    /// Must be called outside of command buffer recording (uses synchronous GPU submits).
    void prepare_accel_structures(const FrameContext& ctx);

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
        float emissive_gi_boost;
        float pad0;
        float pad1;
        float pad2;
        glm::vec3 sdf_volume_min;
        float pad3;
        glm::vec3 sdf_volume_max;
        float pad4;
    };

    /// Push constants for the clipmap reallocation shader. `phase` selects
    /// which of the three dispatches this is; all run over the same cell
    /// grid. `prev_origin` is the level origin the old mapping was built
    /// for — the shader converts old cells to world positions with it, so
    /// no cell-space delta has to be trusted across the boundary.
    struct ClipmapAllocPushConstants {
        glm::vec3 prev_origin;
        uint32_t phase;
        uint32_t full_reset;
    };

    static constexpr uint32_t ALLOC_PHASE_CLEAR = 0;
    static constexpr uint32_t ALLOC_PHASE_REUSE = 1;
    static constexpr uint32_t ALLOC_PHASE_ALLOC = 2;

    /// Max ray distance used in RT trace push constants.
    static constexpr float MAX_TRACE_DIST = 40.0f;

private:
    /// Probe grid bounds expanded by max trace distance — objects outside
    /// this volume can never be hit by DDGI rays.
    [[nodiscard]] AABB trace_reach_bounds() const;

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

    VkDescriptorSetLayout alloc_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout alloc_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline clipmap_alloc_pipeline_{VK_NULL_HANDLE};

    // The clipmap level this pass traces. Pass-level rather than
    // per-viewport: every viewport samples the same world probes, so the
    // level follows the main camera and each viewport's atlases hold the
    // accumulated irradiance for those same cells.
    /// Move this viewport's level to follow its camera, and republish the
    /// resulting origin into the config the UBO is built from. Returns true
    /// if the level moved.
    bool recenter_clipmap(Images& imgs, const glm::vec3& camera_position);

    VkDescriptorSetLayout sample_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout sample_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline sample_pipeline_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    bool is_first_frame_{true};

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

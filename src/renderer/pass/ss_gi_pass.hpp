#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"

#include "renderer/renderer_constants.hpp"
#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

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

/// GPU-side constants UBO for the SS-GI trace shader.
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
    /// Per-viewport resolution-dependent image state.
    struct Images {
        // Raw radiance from RT trace: probe_count.x * (probe_count.y * rays_per_probe)
        VkImage raw_radiance_image{VK_NULL_HANDLE};
        VmaAllocation raw_radiance_memory{VK_NULL_HANDLE};
        VkImageView raw_radiance_view{VK_NULL_HANDLE};

        // Resolved (temporally filtered) probe irradiance: probe_count.x * probe_count.y
        VkImage resolved_image{VK_NULL_HANDLE};
        VmaAllocation resolved_memory{VK_NULL_HANDLE};
        VkImageView resolved_view{VK_NULL_HANDLE};

        // History (previous frame resolved): same size as resolved
        VkImage history_image{VK_NULL_HANDLE};
        VmaAllocation history_memory{VK_NULL_HANDLE};
        VkImageView history_view{VK_NULL_HANDLE};

        // Previous frame's depth, full-screen res. Copied from the scene depth
        // each frame after the resolve reads it, so next frame's resolve can
        // reject history on depth change (disocclusion).
        VkImage prev_depth_image{VK_NULL_HANDLE};
        VmaAllocation prev_depth_memory{VK_NULL_HANDLE};
        VkImageView prev_depth_view{VK_NULL_HANDLE};
        VkExtent2D depth_extent{};
        bool prev_depth_valid{false};

        // Upsampled indirect output: half-res screen
        VkImage indirect_image{VK_NULL_HANDLE};
        VmaAllocation indirect_memory{VK_NULL_HANDLE};
        VkImageView indirect_view{VK_NULL_HANDLE};
        VkExtent2D indirect_extent{};

        // Constants UBO, one per frame in flight. It is host-written every
        // frame, and a frame's trace and resolve may still be pending on the
        // GPU while the CPU records the next one, so they must not share it.
        std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> constants_ubo;

        // Probe dimensions
        glm::uvec2 probe_count{};
        uint32_t rays_per_probe{16};

        // Descriptor pool + sets. The trace and resolve sets hold resources
        // that belong to one frame slot — the constants UBO, the TLAS, the
        // per-instance table — and are rewritten every frame. They are
        // update-after-bind, so rewriting a set that a pending frame still
        // has bound would hand that frame this frame's slot: a TLAS built
        // three frames ago against an instance table just overwritten with
        // a different layout. One set per slot keeps each frame on its own.
        // The upsample set binds only per-viewport images and stays single.
        VkDescriptorPool desc_pool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> trace_set{};
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> resolve_set{};
        VkDescriptorSet upsample_set{VK_NULL_HANDLE};

        bool first_frame{true};

        // Reprojection matrices for the temporal resolve: this frame's
        // inverse view-projection and the previous frame's view-projection.
        // Per-viewport, since each viewport has its own camera history.
        glm::mat4 inv_view_proj{1.0f};
        glm::mat4 prev_view_proj{1.0f};
        glm::mat4 prev_inv_view_proj{1.0f};
        bool history_valid{false};

        void create(VkDevice device, VmaAllocator allocator,
                     const SsGiConfig& config,
                     uint32_t screen_w, uint32_t screen_h);
        void resize(VkDevice device, VmaAllocator allocator,
                     const SsGiConfig& config,
                     uint32_t screen_w, uint32_t screen_h);
        void destroy(VkDevice device, VmaAllocator allocator);

        // ImGui debug textures
        VkDescriptorSet indirect_imgui_desc{VK_NULL_HANDLE};
        [[nodiscard]] VkDescriptorSet indirect_imgui_texture(VkSampler sampler);
        void invalidate_imgui_texture();
    };

    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;

    void record_into(const FrameContext& ctx, Images& imgs);

    void set_config(const SsGiConfig& config) { config_ = config; }
    [[nodiscard]] SsGiConfig& config() { return config_; }
    [[nodiscard]] const SsGiConfig& config() const { return config_; }

    void set_accel_manager(AccelStructureManager* mgr) { accel_manager_ = mgr; }

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

    [[nodiscard]] VkSampler linear_sampler() const { return linear_sampler_; }

    struct TracePushConstants {
        uint32_t rays_per_probe;
        uint32_t ray_offset;
    };

    struct ResolvePushConstants {
        glm::uvec2 probe_count;
        uint32_t rays_per_probe;
        uint32_t frame_index;
        glm::vec2 resolution;
        uint32_t probe_spacing;
        float hysteresis;
    };

    struct UpsamplePushConstants {
        glm::uvec2 output_size;
        glm::uvec2 probe_count;
        uint32_t probe_spacing;
        float pad0;
    };

private:
    void update_constants(Images& imgs, const FrameContext& ctx);

    Device* device_{nullptr};
    VkDevice vk_device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{nullptr};
    SsGiConfig config_;
    uint32_t frame_counter_{0};
    glm::mat4 prev_view_proj_{1.0f};

    // Cached sun light data (set via set_sun_light before recording)
    glm::vec3 sun_direction_{0.0f, -1.0f, 0.0f};
    glm::vec3 sun_color_{1.0f};
    float sun_intensity_{1.5f};

    // RT trace pipeline
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};

    // Resolve compute pipeline
    VkDescriptorSetLayout resolve_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout resolve_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline resolve_pipeline_{VK_NULL_HANDLE};

    // Upsample compute pipeline
    VkDescriptorSetLayout upsample_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout upsample_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline upsample_pipeline_{VK_NULL_HANDLE};

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkSampler nearest_sampler_{VK_NULL_HANDLE};

    // RT backend
    AccelStructureManager* accel_manager_{nullptr};
    float trace_cell_size_{0.0F};

    // Shader binding table
    std::unique_ptr<Buffer> sbt_buffer_;
    VkStridedDeviceAddressRegionKHR sbt_raygen_{};
    VkStridedDeviceAddressRegionKHR sbt_miss_{};
    VkStridedDeviceAddressRegionKHR sbt_hit_{};
    VkStridedDeviceAddressRegionKHR sbt_callable_{};
};

} // namespace fjell

#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/buffer.hpp"

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

struct DdgiConfig {
    glm::ivec3 probe_counts{8, 4, 8};
    glm::vec3 probe_spacing{2.0f};
    glm::vec3 grid_position{-7.0f, -1.0f, -7.0f};  // centered around origin
    float hysteresis{0.85f};
    int probe_rays{256};
    int irradiance_side_length{6};
    int visibility_side_length{6};
    float self_shadow_bias{0.3f};
    float max_probe_offset{0.5f};
    float gi_intensity{1.0f};
    float emissive_gi_boost{1.0f};
    bool half_res_output{true};
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
    float pad0{0.0f};

    glm::mat4 random_rotation{1.0f};

    // Camera data for sampling pass
    glm::vec3 camera_position;
    float pad1{0.0f};

    glm::mat4 inv_view_proj{1.0f};

    glm::vec2 resolution;
    float pad2{0.0f};
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

        bool images_need_init{true};

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

    /// Push constants for the RT trace shader.
    struct RtTracePushConstants {
        glm::vec3 light_direction;
        float light_intensity;
        glm::vec3 light_color;
        float max_trace_dist;
        float emissive_gi_boost;
    };

private:
    void update_constants(Images& imgs, const FrameContext& ctx);

    Device* device_{nullptr};
    VkDevice vk_device_{VK_NULL_HANDLE};
    DdgiConfig config_;
    uint32_t frame_counter_{0};

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

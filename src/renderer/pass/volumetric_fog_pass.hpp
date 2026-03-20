#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>

#include "renderer/renderer_constants.hpp"

namespace fjell {

class VolumetricFogPass {
public:
    void create(VkDevice device, VkPhysicalDevice physical_device,
                VkDescriptorSetLayout global_set_layout);
    void destroy();
    void resize(uint32_t width, uint32_t height);

    void dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                  VkImageView depth_view, VkImageView color_view,
                  uint32_t width, uint32_t height, uint32_t frame_index);

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    // Parameters (public for direct ImGui access)
    bool enabled_{false};
    float density_{0.003f};
    float height_{0.0f};
    float height_falloff_{0.5f};
    float scattering_{0.5f};
    float anisotropy_{0.7f};
    float max_distance_{130.0f};
    float temporal_blend_{0.9f};
    glm::vec3 fog_color_{0.8f, 0.85f, 0.9f};

private:
    static constexpr uint32_t TILE_SIZE = 8;
    static constexpr uint32_t DEPTH_SLICES = 64;
    static constexpr uint32_t SCREEN_DIVISOR = 16;
    static constexpr VkFormat VOLUME_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct VolumeImage {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    void create_volumes();
    void destroy_volumes();
    void update_inject_descriptors(uint32_t current);
    void update_temporal_descriptors(uint32_t current);
    void update_integrate_descriptors(uint32_t current);
    void update_composite_descriptors(uint32_t frame, VkImageView depth_view, VkImageView color_view);

    VolumeImage create_volume_image(uint32_t w, uint32_t h, uint32_t d);
    void destroy_volume_image(VolumeImage& img);

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Ping-pong scatter volumes (avoids vkCmdCopyImage each frame)
    VolumeImage scatter_volumes_[2];
    VolumeImage integrated_volume_;

    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkExtent3D volume_extent_{};

    // Inject pass — 2 sets for ping-pong (pre-filled, no runtime updates)
    VkDescriptorSetLayout inject_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout inject_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline inject_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool inject_pool_{VK_NULL_HANDLE};
    VkDescriptorSet inject_sets_[2]{};

    // Temporal pass — 2 sets for ping-pong
    VkDescriptorSetLayout temporal_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout temporal_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline temporal_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool temporal_pool_{VK_NULL_HANDLE};
    VkDescriptorSet temporal_sets_[2]{};

    // Integrate pass — 2 sets for ping-pong
    VkDescriptorSetLayout integrate_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout integrate_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline integrate_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool integrate_pool_{VK_NULL_HANDLE};
    VkDescriptorSet integrate_sets_[2]{};

    // Composite pass — per-frame sets to avoid updating in-flight descriptors
    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool composite_pool_{VK_NULL_HANDLE};
    VkDescriptorSet composite_sets_[MAX_FRAMES_IN_FLIGHT]{};

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    bool volumes_need_init_{true};
    uint32_t frame_counter_{0};
    uint32_t ping_pong_{0};  // current write target (0 or 1)
};

} // namespace fjell

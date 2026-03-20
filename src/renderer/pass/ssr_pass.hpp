#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace fjell {

class SsrPass {
public:
    void create(VkDevice device, VkPhysicalDevice physical_device,
                VkDescriptorSetLayout global_set_layout);
    void destroy();

    void resize(uint32_t width, uint32_t height);

    /// Run all SSR sub-passes: Hi-Z build, trace, composite.
    void dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                  VkImageView depth_view, VkImageView color_view,
                  VkImageView prev_color_view,
                  uint32_t width, uint32_t height);

    [[nodiscard]] VkImageView ssr_view() const { return ssr_view_; }
    [[nodiscard]] VkImage ssr_image() const { return ssr_image_; }

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    void set_enabled(bool e) { enabled_ = e; }
    void set_max_steps(uint32_t s) { max_steps_ = s; }
    void set_max_distance(float d) { max_distance_ = d; }
    void set_thickness(float t) { thickness_ = t; }
    void set_intensity(float i) { intensity_ = i; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] uint32_t max_steps() const { return max_steps_; }
    [[nodiscard]] float max_distance() const { return max_distance_; }
    [[nodiscard]] float thickness() const { return thickness_; }
    [[nodiscard]] float intensity() const { return intensity_; }

private:
    void create_images(uint32_t width, uint32_t height);
    void destroy_images();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Hi-Z mip chain (R32_SFLOAT)
    static constexpr uint32_t MAX_HIZ_MIPS = 12;
    VkImage hiz_image_{VK_NULL_HANDLE};
    VkDeviceMemory hiz_memory_{VK_NULL_HANDLE};
    std::array<VkImageView, MAX_HIZ_MIPS> hiz_mip_views_{}; // per-mip storage write
    VkImageView hiz_sampled_view_{VK_NULL_HANDLE};           // all mips, sampled read
    uint32_t hiz_mip_count_{0};

    // SSR output (RGBA16F: rgb=reflection, a=validity)
    VkImage ssr_image_{VK_NULL_HANDLE};
    VkDeviceMemory ssr_memory_{VK_NULL_HANDLE};
    VkImageView ssr_view_{VK_NULL_HANDLE};

    VkSampler nearest_sampler_{VK_NULL_HANDLE};
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};

    // Hi-Z build descriptors (per-mip)
    VkDescriptorSetLayout hiz_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout hiz_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline hiz_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool hiz_pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_HIZ_MIPS> hiz_sets_{};

    // SSR trace
    VkDescriptorSetLayout trace_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout trace_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline trace_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool trace_pool_{VK_NULL_HANDLE};
    VkDescriptorSet trace_set_{VK_NULL_HANDLE};

    // SSR composite
    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool composite_pool_{VK_NULL_HANDLE};
    VkDescriptorSet composite_set_{VK_NULL_HANDLE};

    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    bool images_need_init_{true};

    // Cached views for lazy descriptor updates
    VkImageView last_depth_view_{VK_NULL_HANDLE};
    VkImageView last_prev_color_view_{VK_NULL_HANDLE};
    VkImageView last_color_view_{VK_NULL_HANDLE};

    // Parameters
    bool enabled_{true};
    uint32_t max_steps_{64};
    float max_distance_{100.0f};
    float thickness_{0.3f};
    float intensity_{1.0f};
};

} // namespace fjell
